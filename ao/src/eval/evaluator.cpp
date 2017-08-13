#include <numeric>
#include <memory>
#include <cmath>

#include "ao/tree/cache.hpp"
#include "ao/tree/tree.hpp"
#include "ao/eval/evaluator.hpp"
#include "ao/eval/clause.hpp"

namespace Kernel {

////////////////////////////////////////////////////////////////////////////////

Evaluator::Evaluator(const Tree root, const std::map<Tree::Id, float>& vs)
    : root_op(root->op)
{
    auto flat = root.ordered();

    // Helper function to create a new clause in the data array
    // The dummy clause (0) is mapped to the first result slot
    std::unordered_map<Tree::Id, Clause::Id> clauses = {{nullptr, 0}};
    Clause::Id id = flat.size();

    // Helper function to make a new function
    std::list<Clause> tape_;
    auto newClause = [&clauses, &id, &tape_](const Tree::Id t)
    {
        tape_.push_front(
                {t->op,
                 id,
                 clauses.at(t->lhs.get()),
                 clauses.at(t->rhs.get())});
    };

    // Write the flattened tree into the tape!
    std::map<Clause::Id, float> constants;
    for (const auto& m : flat)
    {
        // Normal clauses end up in the tape
        if (m->rank > 0)
        {
            newClause(m.id());
        }
        // For constants and variables, record their values so
        // that we can store those values in the result array
        else if (m->op == Opcode::CONST)
        {
            constants[id] = m->value;
        }
        else if (m->op == Opcode::VAR)
        {
            constants[id] = vs.at(m.id());
            vars.left.insert({id, m.id()});
            var_handles.insert({m.id(), m});
        }
        else
        {
            assert(m->op == Opcode::VAR_X ||
                   m->op == Opcode::VAR_Y ||
                   m->op == Opcode::VAR_Z);
        }
        clauses[m.id()] = id--;
    }
    assert(id == 0);

    //  Move from the list tape to a more-compact vector tape
    tapes.push_back(Tape());
    tape = tapes.begin();
    for (auto& t : tape_)
    {
        tape->t.push_back(t);
    }

    // Make sure that X, Y, Z have been allocated space
    std::vector<Tree> axes = {Tree::X(), Tree::Y(), Tree::Z()};
    for (auto a : axes)
    {
        if (clauses.find(a.id()) == clauses.end())
        {
            clauses[a.id()] = clauses.size();
        }
    }

    // Allocate enough memory for all the clauses
    result.reset(new Result(clauses.size() + 1, vars.size()));
    disabled.resize(clauses.size() + 1);
    remap.resize(clauses.size() + 1);

    // Store all constants in results array
    for (auto c : constants)
    {
        result->fill(c.second, c.first);
    }

    // Save X, Y, Z ids
    X = clauses.at(axes[0].id());
    Y = clauses.at(axes[1].id());
    Z = clauses.at(axes[2].id());

    // Set derivatives for X, Y, Z (unchanging)
    result->setDeriv(Eigen::Vector3f::UnitX(), X);
    result->setDeriv(Eigen::Vector3f::UnitY(), Y);
    result->setDeriv(Eigen::Vector3f::UnitZ(), Z);

    {   // Set the Jacobian for our variables (unchanging)
        size_t index = 0;
        for (auto v : vars.left)
        {
            result->setGradient(v.first, index++);
        }
    }

    // Store the index of the tree's root
    assert(clauses.at(root.id()) == 1);
    tape->i = clauses.at(root.id());
}

////////////////////////////////////////////////////////////////////////////////

float Evaluator::eval(const Eigen::Vector3f& p)
{
    set(p, 0);
    return values(1)[0];
}

float Evaluator::baseEval(const Eigen::Vector3f& p)
{
    auto prev_tape = tape;

    // Walk up the tape stack until we find an interval-type tape
    // that contains the given point, or we hit the start of the stack
    while (tape != tapes.begin())
    {
        if (tape->type == Tape::INTERVAL &&
            p.x() >= tape->X.lower() && p.x() <= tape->X.upper() &&
            p.y() >= tape->Y.lower() && p.y() <= tape->Y.upper() &&
            p.z() >= tape->Z.lower() && p.z() <= tape->Z.upper())
        {
            break;
        }
        else
        {
            tape--;
        }
    }

    auto out = eval(p);
    tape = prev_tape;
    return out;
}

Interval::I Evaluator::eval(const Eigen::Vector3f& lower, const Eigen::Vector3f& upper)
{
    set(lower, upper);
    return interval();
}

void Evaluator::set(const Eigen::Vector3f& lower, const Eigen::Vector3f& upper)
{
    result->i[X] = {lower.x(), upper.x()};
    result->i[Y] = {lower.y(), upper.y()};
    result->i[Z] = {lower.z(), upper.z()};
}

////////////////////////////////////////////////////////////////////////////////

void Evaluator::pushTape(Tape::Type t)
{
    auto prev_tape = tape;

    // Add another tape to the top of the tape stack if one doesn't already
    // exist (we never erase them, to avoid re-allocating memory during
    // nested evaluations).
    if (++tape == tapes.end())
    {
        tape = tapes.insert(tape, Tape());
        tape->t.reserve(tapes.front().t.size());
    }
    else
    {
        // We may be reusing an existing tape, so resize to 0
        // (preserving allocated storage)
        tape->t.clear();
    }

    assert(tape != tapes.end());
    assert(tape != tapes.begin());
    assert(tape->t.capacity() >= prev_tape->t.size());

    // Reset tape type
    tape->type = t;

    // Now, use the data in disabled and remap to make the new tape
    for (const auto& c : prev_tape->t)
    {
        if (!disabled[c.id])
        {
            Clause::Id ra, rb;
            for (ra = c.a; remap[ra]; ra = remap[ra]);
            for (rb = c.b; remap[rb]; rb = remap[rb]);
            tape->t.push_back({c.op, c.id, ra, rb});
        }
    }

    // Remap the tape root index
    for (tape->i = prev_tape->i; remap[tape->i]; tape->i = remap[tape->i]);

    // Make sure that the tape got shorter
    assert(tape->t.size() <= prev_tape->t.size());
}

void Evaluator::push()
{
    // Since we'll be figuring out which clauses are disabled and
    // which should be remapped, we reset those arrays here
    std::fill(disabled.begin(), disabled.end(), true);
    std::fill(remap.begin(), remap.end(), 0);

    // Mark the root node as active
    disabled[tape->i] = false;

    for (const auto& c : tape->t)
    {
        if (!disabled[c.id])
        {
            // For min and max operations, we may only need to keep one branch
            // active if it is decisively above or below the other branch.
            if (c.op == Opcode::MAX)
            {
                if (result->i[c.a].lower() > result->i[c.b].upper())
                {
                    disabled[c.a] = false;
                    remap[c.id] = c.a;
                }
                else if (result->i[c.b].lower() > result->i[c.a].upper())
                {
                    disabled[c.b] = false;
                    remap[c.id] = c.b;
                }
            }
            else if (c.op == Opcode::MIN)
            {
                if (result->i[c.a].lower() > result->i[c.b].upper())
                {
                    disabled[c.b] = false;
                    remap[c.id] = c.b;
                }
                else if (result->i[c.b].lower() > result->i[c.a].upper())
                {
                    disabled[c.a] = false;
                    remap[c.id] = c.a;
                }
            }
            if (!remap[c.id])
            {
                disabled[c.a] = false;
                disabled[c.b] = false;
            }
            else
            {
                disabled[c.id] = true;
            }
        }
    }

    pushTape(Tape::INTERVAL);
    tape->X = result->i[X];
    tape->Y = result->i[Y];
    tape->Z = result->i[Z];
}

Feature Evaluator::push(const Feature& f)
{
    // Since we'll be figuring out which clauses are disabled and
    // which should be remapped, we reset those arrays here
    std::fill(disabled.begin(), disabled.end(), true);
    std::fill(remap.begin(), remap.end(), 0);

    // Mark the root node as active
    disabled[tape->i] = false;

    Feature out;
    out.deriv = f.deriv;

    const auto& choices = f.getChoices();
    auto itr = choices.begin();

    for (const auto& c : tape->t)
    {
        const bool match = ((result->f(c.a, 0) == result->f(c.b, 0) || c.a == c.b) &&
                            (c.op == Opcode::MAX || c.op == Opcode::MIN) &&
                            itr != choices.end() && itr->id == c.id);

        if (!disabled[c.id])
        {
            // For ambiguous min and max operations, we obey the feature in
            // terms of which branch to take
            if (match)
            {
                if (f.hasEpsilon(c.id))
                {
                    out.push_raw(*itr, f.getEpsilon(c.id));
                }
                else
                {
                    out.push_choice_raw(*itr);
                }

                if (itr->choice == 0)
                {
                    disabled[c.a] = false;
                    remap[c.id] = c.a;
                }
                else
                {
                    disabled[c.b] = false;
                    remap[c.id] = c.b;
                }
            }

            if (!remap[c.id])
            {
                disabled[c.a] = false;
                disabled[c.b] = false;
            }
            else
            {
                disabled[c.id] = true;
            }
        }

        if (match)
        {
            ++itr;
        }
    }
    assert(itr == choices.end());

    pushTape(Tape::FEATURE);

    return out;
}

void Evaluator::specialize(const Eigen::Vector3f& p)
{
    // Load results into the first floating-point result slot
    eval(p);

    // The same logic as push, but using float instead of interval comparisons
    std::fill(disabled.begin(), disabled.end(), true);
    std::fill(remap.begin(), remap.end(), 0);

    // Mark the root node as active
    disabled[tape->i] = false;

    for (const auto& c : tape->t)
    {
        if (!disabled[c.id])
        {
            // For min and max operations, we may only need to keep one branch
            // active if it is decisively above or below the other branch.
            if (c.op == Opcode::MAX)
            {
                if (result->f(c.a, 0) > result->f(c.b, 0))
                {
                    disabled[c.a] = false;
                    remap[c.id] = c.a;
                }
                else if (result->f(c.b, 0) > result->f(c.a, 0))
                {
                    disabled[c.b] = false;
                    remap[c.id] = c.b;
                }
            }
            else if (c.op == Opcode::MIN)
            {
                if (result->f(c.a, 0) > result->f(c.b, 0))
                {
                    disabled[c.b] = false;
                    remap[c.id] = c.b;
                }
                else if (result->f(c.b, 0) > result->f(c.a, 0))
                {
                    disabled[c.a] = false;
                    remap[c.id] = c.a;
                }
            }
            if (!remap[c.id])
            {
                disabled[c.a] = false;
                disabled[c.b] = false;
            }
            else
            {
                disabled[c.id] = true;
            }
        }
    }

    pushTape(Tape::SPECIALIZED);
}

bool Evaluator::isInside(const Eigen::Vector3f& p)
{
    set(p, 0);
    auto ds = derivs(1);
    auto vs = ds.v;

    // Unambiguous cases
    if (vs[0] < 0)
    {
        return true;
    }
    else if (vs[0] > 0)
    {
        return false;
    }

    // Special case to save time on non-ambiguous features: we can get both
    // positive and negative values out if there's a non-zero gradient
    // (same as single-feature case below).
    if (!isAmbiguous())
    {
        return (ds.dx[0] != 0) || (ds.dy[0] != 0) || (ds.dz[0] != 0);
    }

    // Otherwise, we need to handle the zero-crossing case!

    // First, we extract all of the features
    auto fs = featuresAt(p);

    // If there's only a single feature, we can get both positive and negative
    // values out if it's got a non-zero gradient
    if (fs.size() == 1)
    {
        return fs.front().deriv.norm() > 0;
    }

    // Otherwise, check each feature
    // The only case where we're outside the model is if all features
    // and their normals are all positive (i.e. for every epsilon that
    // we move from (x,y,z), epsilon . deriv > 0)
    bool pos = false;
    bool neg = false;
    for (auto& f : fs)
    {
        pos |= f.isCompatible(f.deriv);
        neg |= f.isCompatible(-f.deriv);
    }
    return !(pos && !neg);
}

std::list<Feature> Evaluator::featuresAt(const Eigen::Vector3f& p)
{
    // The initial feature doesn't know any ambiguities
    Feature f;
    std::list<Feature> todo = {f};
    std::list<Feature> done;
    std::set<std::list<Feature::Choice>> seen;

    // Load the location into the first results slot and evaluate
    specialize(p);

    while (todo.size())
    {
        // Take the most recent feature and scan for ambiguous min/max nodes
        // (from the bottom up).  If we find such an ambiguous node, then push
        // both versions to the feature (if compatible) and re-insert the
        // augmented feature in the todo list; otherwise, move the feature
        // to the done list.
        auto f = todo.front();
        todo.pop_front();

        // Then, push into this feature
        // (storing a minimized version of the feature)
        auto f_ = push(f);

        // Run a single evaluation of the value + derivatives
        // The value will be the same, but derivatives may change
        // depending on which feature we've pushed ourselves into
        const auto ds = derivs(1);

        bool ambiguous = false;
        for (auto itr = tape->t.rbegin(); itr != tape->t.rend() && !ambiguous;
                ++itr)
        {
            if ((itr->op == Opcode::MIN || itr->op == Opcode::MAX))
            {
                // If we've ended up with a non-selection, then collapse
                // it to a single choice
                if (itr->a == itr->b)
                {
                    auto fa = f_;
                    fa.push_choice({itr->id, 0});
                    todo.push_back(fa);
                    ambiguous = true;
                }
                // Check for ambiguity here
                else if (result->f(itr->a, 0) == result->f(itr->b, 0))
                {
                    // Check both branches of the ambiguity
                    const Eigen::Vector3d rhs(result->dx(itr->b, 0),
                                              result->dy(itr->b, 0),
                                              result->dz(itr->b, 0));
                    const Eigen::Vector3d lhs(result->dx(itr->a, 0),
                                              result->dy(itr->a, 0),
                                              result->dz(itr->a, 0));
                    const auto epsilon = (itr->op == Opcode::MIN) ? (rhs - lhs)
                                                                  : (lhs - rhs);

                    auto fa = f_;
                    if (fa.push(epsilon, {itr->id, 0}))
                    {
                        ambiguous = true;
                        todo.push_back(fa);
                    }

                    auto fb = f_;
                    if (fb.push(-epsilon, {itr->id, 1}))
                    {
                        ambiguous = true;
                        todo.push_back(fb);
                    }
                }
            }
        }

        if (!ambiguous)
        {
            f_.deriv = {ds.dx[0], ds.dy[0], ds.dz[0]};
            if (seen.find(f_.getChoices()) == seen.end())
            {
                seen.insert(f_.getChoices());
                done.push_back(f_);
            }
        }
        pop(); // push(Feature)
    }
    pop(); // specialization

    assert(done.size() > 0);
    return done;
}

bool Evaluator::isAmbiguous(const Eigen::Vector3f& p)
{
    eval(p);
    return isAmbiguous();
}

bool Evaluator::isAmbiguous()
{
    for (const auto& c : tape->t)
    {
        if ((c.op == Opcode::MIN || c.op == Opcode::MAX) &&
            result->f(c.a, 0) == result->f(c.b, 0))
        {
            return true;
        }
    }
    return false;
}

std::set<Result::Index> Evaluator::getAmbiguous(Result::Index i) const
{
    std::set<Result::Index> out;
    for (const auto& c : tape->t)
    {
        if (c.op == Opcode::MIN || c.op == Opcode::MAX)
        {
            for (Result::Index j=0; j < i; ++j)
            {
                if (result->f(c.a, j) == result->f(c.b, j))
                {
                    out.insert(j);
                }
            }
        }
    }
    return out;
}

void Evaluator::pop()
{
    assert(tape != tapes.begin());
    tape--;
}

////////////////////////////////////////////////////////////////////////////////

#define JAC_LOOP for (auto a = aj.begin(), b = bj.begin(), o = oj.begin(); a != aj.end(); ++a, ++b, ++o)
void Evaluator::eval_clause_jacobians(Opcode::Opcode op,
    const float av,  std::vector<float>& aj,
    const float bv,  std::vector<float>& bj,
    std::vector<float>& oj)
{
    switch (op) {
        case Opcode::ADD:
            JAC_LOOP
            {
                (*o) = (*a) + (*b);
            }
            break;
        case Opcode::MUL:
            JAC_LOOP
            {   // Product rule
                (*o) = av * (*b) + bv * (*a);
            }
            break;
        case Opcode::MIN:
            JAC_LOOP
            {
                if (av < bv)
                {
                    (*o) = (*a);
                }
                else
                {
                    (*o) = (*b);
                }
            }
            break;
        case Opcode::MAX:
            JAC_LOOP
            {
                if (av < bv)
                {
                    (*o) = (*b);
                }
                else
                {
                    (*o) = (*a);
                }
            }
            break;
        case Opcode::SUB:
            JAC_LOOP
            {
                (*o) = (*a) - (*b);
            }
            break;
        case Opcode::DIV:
            JAC_LOOP
            {
                const float p = pow(bv, 2);
                (*o) = (bv*(*a) - av*(*b)) / p;
            }
            break;
        case Opcode::ATAN2:
            JAC_LOOP
            {
                const float d = pow(av, 2) + pow(bv, 2);
                (*o) = ((*a)*bv - av*(*b)) / d;
            }
            break;
        case Opcode::POW:
            JAC_LOOP
            {
                const float m = pow(av, bv - 1);

                // The full form of the derivative is
                // (*o) = m * (bv * (*a) + av * log(av) * (*b)))
                // However, log(av) is often NaN and (*b) is always zero,
                // (since it must be CONST), so we skip that part.
                (*o) = m * (bv * (*a));
            }
            break;
        case Opcode::NTH_ROOT:
            JAC_LOOP
            {
                const float m = pow(av, 1.0f/bv - 1);
                (*o) = m * (1.0f/bv * (*a));
            }
            break;
        case Opcode::MOD:
            JAC_LOOP
            {
                // This isn't quite how partial derivatives of mod work,
                // but close enough normals rendering.
                (*o) = (*a);
            }
            break;
        case Opcode::NANFILL:
            JAC_LOOP
            {
                (*o) = std::isnan(av) ? (*b) : (*a);
            }
            break;

        case Opcode::SQUARE:
            JAC_LOOP
            {
                (*o) = 2 * av * (*a);
            }
            break;
        case Opcode::SQRT:
            JAC_LOOP
            {
                if (av < 0)
                {
                    (*o) = 0;
                }
                else
                {
                    (*o) = (*a) / (2 * sqrt(av));
                }
            }
            break;
        case Opcode::NEG:
            JAC_LOOP
            {
                (*o) = -(*a);
            }
            break;
        case Opcode::SIN:
            JAC_LOOP
            {
                const float c = cos(av);
                (*o) = (*a) * c;
            }
            break;
        case Opcode::COS:
            JAC_LOOP
            {
                const float s = -sin(av);
                (*o) = (*a) * s;
            }
            break;
        case Opcode::TAN:
            JAC_LOOP
            {
                const float s = pow(1/cos(av), 2);
                (*o) = (*a) * s;
            }
            break;
        case Opcode::ASIN:
                JAC_LOOP
                {
                    const float d = sqrt(1 - pow(av, 2));
                    (*o) = (*a) / d;
                }
                break;
            case Opcode::ACOS:
                JAC_LOOP
                {
                    const float d = -sqrt(1 - pow(av, 2));
                    (*o) = (*a) / d;
                }
                break;
            case Opcode::ATAN:
                JAC_LOOP
                {
                    const float d = pow(av, 2) + 1;
                    (*o) = (*a) / d;
                }
                break;
            case Opcode::EXP:
                JAC_LOOP
                {
                    const float e = exp(av);
                    (*o) = e * (*a);
                }
                break;

            case Opcode::CONST_VAR:
                JAC_LOOP
                {
                    (*o) = 0;
                }
                break;

            case Opcode::INVALID:
            case Opcode::CONST:
            case Opcode::VAR_X:
            case Opcode::VAR_Y:
            case Opcode::VAR_Z:
            case Opcode::VAR:
            case Opcode::LAST_OP: assert(false);
        }
}

Interval::I Evaluator::eval_clause_interval(
        Opcode::Opcode op, const Interval::I& a, const Interval::I& b)
{
    switch (op) {
        case Opcode::ADD:
            return a + b;
        case Opcode::MUL:
            return a * b;
        case Opcode::MIN:
            return boost::numeric::min(a, b);
        case Opcode::MAX:
            return boost::numeric::max(a, b);
        case Opcode::SUB:
            return a - b;
        case Opcode::DIV:
            return a / b;
        case Opcode::ATAN2:
            return atan2(a, b);
        case Opcode::POW:
            return boost::numeric::pow(a, b.lower());
        case Opcode::NTH_ROOT:
            return boost::numeric::nth_root(a, b.lower());
        case Opcode::MOD:
            return Interval::I(0.0f, b.upper()); // YOLO
        case Opcode::NANFILL:
            return (std::isnan(a.lower()) || std::isnan(a.upper())) ? b : a;

        case Opcode::SQUARE:
            return boost::numeric::square(a);
        case Opcode::SQRT:
            return boost::numeric::sqrt(a);
        case Opcode::NEG:
            return -a;
        case Opcode::SIN:
            return boost::numeric::sin(a);
        case Opcode::COS:
            return boost::numeric::cos(a);
        case Opcode::TAN:
            return boost::numeric::tan(a);
        case Opcode::ASIN:
            return boost::numeric::asin(a);
        case Opcode::ACOS:
            return boost::numeric::acos(a);
        case Opcode::ATAN:
            return boost::numeric::atan(a);
        case Opcode::EXP:
            return boost::numeric::exp(a);

        case Opcode::CONST_VAR:
            return a;

        case Opcode::INVALID:
        case Opcode::CONST:
        case Opcode::VAR_X:
        case Opcode::VAR_Y:
        case Opcode::VAR_Z:
        case Opcode::VAR:
        case Opcode::LAST_OP: assert(false);
    }
    return Interval::I();
}

////////////////////////////////////////////////////////////////////////////////

const float* Evaluator::values(Result::Index count)
{
    for (auto itr = tape->t.rbegin(); itr != tape->t.rend(); ++itr)
    {
#define out result->f.row(itr->id).head(count)
#define a result->f.row(itr->a).head(count)
#define b result->f.row(itr->b).head(count)
        switch (itr->op) {
            case Opcode::ADD:
                out = a + b;
                break;
            case Opcode::MUL:
                out = a * b;
                break;
            case Opcode::MIN:
                out = a.cwiseMin(b);
                break;
            case Opcode::MAX:
                out = a.cwiseMax(b);
                break;
            case Opcode::SUB:
                out = a - b;
                break;
            case Opcode::DIV:
                out = a / b;
                break;
            case Opcode::ATAN2:
                for (auto i=0; i < a.size(); ++i)
                {
                    out(i) = atan2(a(i), b(i));
                }
                break;
            case Opcode::POW:
                out = a.pow(b);
                break;
            case Opcode::NTH_ROOT:
                out = pow(a, 1.0f/b);
                break;
            case Opcode::MOD:
                for (auto i=0; i < a.size(); ++i)
                {
                    out(i) = std::fmod(a(i), b(i));
                    while (out(i) < 0)
                    {
                        out(i) += b(i);
                    }
                }
                break;
            case Opcode::NANFILL:
                out = a.isNaN().select(b, a);
                break;

            case Opcode::SQUARE:
                out = a * a;
                break;
            case Opcode::SQRT:
                out = sqrt(a);
                break;
            case Opcode::NEG:
                out = -a;
                break;
            case Opcode::SIN:
                out = sin(a);
                break;
            case Opcode::COS:
                out = cos(a);
                break;
            case Opcode::TAN:
                out = tan(a);
                break;
            case Opcode::ASIN:
                out = asin(a);
                break;
            case Opcode::ACOS:
                out = acos(a);
                break;
            case Opcode::ATAN:
                out = atan(a);
                break;
            case Opcode::EXP:
                out = exp(a);
                break;

            case Opcode::CONST_VAR:
                out = a;
                break;

            case Opcode::INVALID:
            case Opcode::CONST:
            case Opcode::VAR_X:
            case Opcode::VAR_Y:
            case Opcode::VAR_Z:
            case Opcode::VAR:
            case Opcode::LAST_OP: assert(false);
        }

#undef out
#undef a
#undef b
    }

    return &result->f(tape->i, 0);
}

Evaluator::Derivs Evaluator::derivs(Result::Index count)
{
    values(count);

    for (auto itr = tape->t.rbegin(); itr != tape->t.rend(); ++itr)
    {

#define ov result->f.row(itr->id).head(count)
#define odx result->dx.row(itr->id).head(count)
#define ody result->dy.row(itr->id).head(count)
#define odz result->dz.row(itr->id).head(count)

#define av  result->f.row(itr->a).head(count)
#define adx result->dx.row(itr->a).head(count)
#define ady result->dy.row(itr->a).head(count)
#define adz result->dz.row(itr->a).head(count)

#define bv  result->f.row(itr->b).head(count)
#define bdx result->dx.row(itr->b).head(count)
#define bdy result->dy.row(itr->b).head(count)
#define bdz result->dz.row(itr->b).head(count)

        switch (itr->op) {
            case Opcode::ADD:
                odx = adx + bdx;
                ody = ady + bdy;
                odz = adz + bdz;
                break;
            case Opcode::MUL:
                // Product rule
                odx = av*bdx + adx*bv;
                ody = av*bdy + ady*bv;
                odz = av*bdz + adz*bv;
                break;
            case Opcode::MIN:
                odx = (av < bv).select(adx, bdx);
                ody = (av < bv).select(ady, bdy);
                odz = (av < bv).select(adz, bdz);
                break;
            case Opcode::MAX:
                odx = (av < bv).select(bdx, adx);
                ody = (av < bv).select(bdy, ady);
                odz = (av < bv).select(bdz, adz);
                break;
            case Opcode::SUB:
                odx = adx - bdx;
                ody = ady - bdy;
                odz = adz - bdz;
                break;
            case Opcode::DIV:
                odz = bv.pow(2); // Temporary
                odx = (bv*adx - av*bdx) / odz;
                ody = (bv*ady - av*bdy) / odz;
                odz = (bv*adz - av*bdz) / odz;
                break;
            case Opcode::ATAN2:
                odz = av.pow(2) + bv.pow(2); // Temporary
                odx = (adx*bv - av*bdx) / odz;
                ody = (ady*bv - av*bdy) / odz;
                odz = (adz*bv - av*bdz) / odz;
                break;
            case Opcode::POW:
                odz = av.pow(bv - 1); // Temporary

                // The full form of the derivative is
                // odx = m * (bv * adx + av * log(av) * bdx))
                // However, log(av) is often NaN and bdx is always zero,
                // (since it must be CONST), so we skip that part.
                odx = odz * (bv * adx);
                ody = odz * (bv * ady);
                odz = odz * (bv * adz);
                break;

            case Opcode::NTH_ROOT:
                odz = 1.0f / bv;    // Temporary
                odz = av.pow(odz - 1) * odz; // Temporary
                odx = odz * adx;
                ody = odz * ady;
                odz = odz * adz;
                break;
            case Opcode::MOD:
                odx = adx;
                ody = ady;
                odz = adz;
                break;
            case Opcode::NANFILL:
                odx = av.isNaN().select(bdx, adx);
                ody = av.isNaN().select(bdy, ady);
                odz = av.isNaN().select(bdz, adz);
                break;

            case Opcode::SQUARE:
                odz = 2 * av; // Temporary
                odx = odz * adx;
                ody = odz * ady;
                odz = odz * adz;
                break;
            case Opcode::SQRT:
                // TODO: this could be more efficient
                odx = (av < 0).select(0, adx / (2 * ov));
                ody = (av < 0).select(0, ady / (2 * ov));
                odz = (av < 0).select(0, adz / (2 * ov));
                break;
            case Opcode::NEG:
                odx = -adx;
                ody = -ady;
                odz = -adz;
                break;
            case Opcode::SIN:
                odz = cos(av); // Temporary
                odx = adx * odz;
                ody = ady * odz;
                odz = adz * odz;
                break;
            case Opcode::COS:
                odz = -sin(av); // Temporary
                odx = adx * odz;
                ody = ady * odz;
                odz = adz * odz;
                break;
            case Opcode::TAN:
                odz = pow(1/cos(av), 2);
                odx = adx * odz;
                ody = ady * odz;
                odz = adz * odz;
                break;
            case Opcode::ASIN:
                odz = sqrt(1 - pow(av, 2)); // Temporary
                odx = adx / odz;
                ody = ady / odz;
                odz = adz / odz;
                break;
            case Opcode::ACOS:
                odz = -sqrt(1 - pow(av, 2)); // Temporary
                odx = adx / odz;
                ody = ady / odz;
                odz = adz / odz;
                break;
            case Opcode::ATAN:
                odz = pow(av, 2) + 1; // Temporary
                odx = adx / odz;
                ody = ady / odz;
                odz = adz / odz;
                break;
            case Opcode::EXP:
                odz = exp(av); // Temporary
                odx = odz * adx;
                ody = odz * ady;
                odz = odz * adz;
                break;

            case Opcode::CONST_VAR:
                odx = adx;
                ody = ady;
                odz = adz;
                break;

            case Opcode::INVALID:
            case Opcode::CONST:
            case Opcode::VAR_X:
            case Opcode::VAR_Y:
            case Opcode::VAR_Z:
            case Opcode::VAR:
            case Opcode::LAST_OP: assert(false);
        }

#undef ov
#undef odx
#undef ody
#undef odz

#undef av
#undef adx
#undef ady
#undef adz

#undef bv
#undef bdx
#undef bdy
#undef bdz
    }
    return { &result->f(tape->i, 0),  &result->dx(tape->i, 0),
             &result->dy(tape->i, 0), &result->dz(tape->i, 0) };
}

std::map<Tree::Id, float> Evaluator::gradient(const Eigen::Vector3f& p)
{
    // Fill the values before solving for jacobians
    set(p, 0);
    values(1);

    for (auto itr = tape->t.rbegin(); itr != tape->t.rend(); ++itr)
    {
        float av = result->f(itr->a, 0);
        float bv = result->f(itr->b, 0);
        std::vector<float>& aj = result->j[itr->a];
        std::vector<float>& bj = result->j[itr->b];

        eval_clause_jacobians(itr->op, av, aj, bv, bj, result->j[itr->id]);
    }

    std::map<Tree::Id, float> out;
    {   // Unpack from flat array into map
        // (to allow correlating back to VARs in Tree)
        const auto ti = tape->i;
        size_t index = 0;
        for (auto v : vars.left)
        {
            out[v.second] = result->j[ti][index++];
        }
    }
    return out;
}

Interval::I Evaluator::interval()
{
    for (auto itr = tape->t.rbegin(); itr != tape->t.rend(); ++itr)
    {
        result->i[itr->id] = eval_clause_interval(itr->op,
                result->i[itr->a], result->i[itr->b]);
    }
    return result->i[tape->i];
}

////////////////////////////////////////////////////////////////////////////////

double Evaluator::utilization() const
{
    return tape->t.size() / double(tapes.front().t.size());
}

void Evaluator::setVar(Tree::Id var, float value)
{
    auto r = vars.right.find(var);
    if (r != vars.right.end())
    {
        result->setValue(value, r->second);
    }
}

std::map<Tree::Id, float> Evaluator::varValues() const
{
    std::map<Tree::Id, float> out;

    for (auto v : vars.left)
    {
        out[v.second] = result->f(v.first, 0);
    }
    return out;
}

bool Evaluator::updateVars(const std::map<Kernel::Tree::Id, float>& vars_)
{
    bool changed = false;
    for (const auto& v : vars.left)
    {
        auto val = vars_.at(v.second);
        if (val != result->f(v.first, 0))
        {
            setVar(v.second, val);
            changed = true;
        }
    }
    return changed;
}

}   // namespace Kernel
