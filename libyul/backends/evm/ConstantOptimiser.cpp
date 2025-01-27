/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Optimisation stage that replaces constants by expressions that compute them.
 */

#include <libyul/backends/evm/ConstantOptimiser.h>

#include <libyul/optimiser/ASTCopier.h>
#include <libyul/backends/evm/EVMMetrics.h>
#include <libyul/AsmData.h>
#include <libyul/Utilities.h>

#include <libdevcore/CommonData.h>

using namespace std;
using namespace dev;
using namespace yul;

using Representation = ConstantOptimiser::Representation;

namespace
{
struct MiniEVMInterpreter: boost::static_visitor<u256>
{
	explicit MiniEVMInterpreter(EVMDialect const& _dialect): m_dialect(_dialect) {}

	u256 eval(Expression const& _expr)
	{
		return std::apply_visitor(*this, _expr);
	}

	u256 eval(dev::eth::Instruction _instr, vector<Expression> const& _arguments)
	{
		vector<u256> args;
		for (auto const& arg: _arguments)
			args.emplace_back(eval(arg));
		switch (_instr)
		{
		case eth::Instruction::ADD:
			return args.at(0) + args.at(1);
		case eth::Instruction::SUB:
			return args.at(0) - args.at(1);
		case eth::Instruction::MUL:
			return args.at(0) * args.at(1);
		case eth::Instruction::EXP:
			return exp256(args.at(0), args.at(1));
		case eth::Instruction::SHL:
			return args.at(0) > 255 ? 0 : (args.at(1) << unsigned(args.at(0)));
		case eth::Instruction::NOT:
			return ~args.at(0);
		default:
			yulAssert(false, "Invalid operation generated in constant optimizer.");
		}
		return 0;
	}

	u256 operator()(FunctionalInstruction const& _instr)
	{
		return eval(_instr.instruction, _instr.arguments);
	}
	u256 operator()(FunctionCall const& _funCall)
	{
		BuiltinFunctionForEVM const* fun = m_dialect.builtin(_funCall.functionName.name);
		yulAssert(fun, "Expected builtin function.");
		yulAssert(fun->instruction, "Expected EVM instruction.");
		return eval(*fun->instruction, _funCall.arguments);
	}
	u256 operator()(Literal const& _literal)
	{
		return valueOfLiteral(_literal);
	}
	u256 operator()(Identifier const&) { yulAssert(false, ""); }

	EVMDialect const& m_dialect;
};
}

void ConstantOptimiser::visit(Expression& _e)
{
	if (_e.type() == typeid(Literal))
	{
		Literal const& literal = boost::get<Literal>(_e);
		if (literal.kind != LiteralKind::Number)
			return;

		if (
			Expression const* repr =
				RepresentationFinder(m_dialect, m_meter, locationOf(_e), m_cache)
				.tryFindRepresentation(valueOfLiteral(literal))
		)
			_e = ASTCopier{}.translate(*repr);
	}
	else
		ASTModifier::visit(_e);
}

Expression const* RepresentationFinder::tryFindRepresentation(dev::u256 const& _value)
{
	if (_value < 0x10000)
		return nullptr;

	Representation const& repr = findRepresentation(_value);
	if (repr.expression->type() == typeid(Literal))
		return nullptr;
	else
		return repr.expression.get();
}

Representation const& RepresentationFinder::findRepresentation(dev::u256 const& _value)
{
	if (m_cache.count(_value))
		return m_cache.at(_value);

	Representation routine = represent(_value);

	if (dev::bytesRequired(~_value) < dev::bytesRequired(_value))
		// Negated is shorter to represent
		routine = min(move(routine), represent("not"_yulstring, findRepresentation(~_value)));

	// Decompose value into a * 2**k + b where abs(b) << 2**k
	for (unsigned bits = 255; bits > 8 && m_maxSteps > 0; --bits)
	{
		unsigned gapDetector = unsigned((_value >> (bits - 8)) & 0x1ff);
		if (gapDetector != 0xff && gapDetector != 0x100)
			continue;

		u256 powerOfTwo = u256(1) << bits;
		u256 upperPart = _value >> bits;
		bigint lowerPart = _value & (powerOfTwo - 1);
		if ((powerOfTwo - lowerPart) < lowerPart)
		{
			lowerPart = lowerPart - powerOfTwo; // make it negative
			upperPart++;
		}
		if (upperPart == 0)
			continue;
		if (abs(lowerPart) >= (powerOfTwo >> 8))
			continue;
		Representation newRoutine;
		if (m_dialect.evmVersion().hasBitwiseShifting())
			newRoutine = represent("shl"_yulstring, represent(bits), findRepresentation(upperPart));
		else
		{
			newRoutine = represent("exp"_yulstring, represent(2), represent(bits));
			if (upperPart != 1)
				newRoutine = represent("mul"_yulstring, findRepresentation(upperPart), newRoutine);
		}

		if (newRoutine.cost >= routine.cost)
			continue;

		if (lowerPart > 0)
			newRoutine = represent("add"_yulstring, newRoutine, findRepresentation(u256(abs(lowerPart))));
		else if (lowerPart < 0)
			newRoutine = represent("sub"_yulstring, newRoutine, findRepresentation(u256(abs(lowerPart))));

		if (m_maxSteps > 0)
			m_maxSteps--;
		routine = min(move(routine), move(newRoutine));
	}
	yulAssert(MiniEVMInterpreter{m_dialect}.eval(*routine.expression) == _value, "Invalid expression generated.");
	return m_cache[_value] = move(routine);
}

Representation RepresentationFinder::represent(dev::u256 const& _value) const
{
	Representation repr;
	repr.expression = make_unique<Expression>(Literal{m_location, LiteralKind::Number, YulString{formatNumber(_value)}, {}});
	repr.cost = m_meter.costs(*repr.expression);
	return repr;
}

Representation RepresentationFinder::represent(
	YulString _instruction,
	Representation const& _argument
) const
{
	Representation repr;
	repr.expression = make_unique<Expression>(FunctionCall{
		m_location,
		Identifier{m_location, _instruction},
		{ASTCopier{}.translate(*_argument.expression)}
	});
	repr.cost = _argument.cost + m_meter.instructionCosts(*m_dialect.builtin(_instruction)->instruction);
	return repr;
}

Representation RepresentationFinder::represent(
	YulString _instruction,
	Representation const& _arg1,
	Representation const& _arg2
) const
{
	Representation repr;
	repr.expression = make_unique<Expression>(FunctionCall{
		m_location,
		Identifier{m_location, _instruction},
		{ASTCopier{}.translate(*_arg1.expression), ASTCopier{}.translate(*_arg2.expression)}
	});
	repr.cost = m_meter.instructionCosts(*m_dialect.builtin(_instruction)->instruction) + _arg1.cost + _arg2.cost;
	return repr;
}

Representation RepresentationFinder::min(Representation _a, Representation _b)
{
	if (_a.cost <= _b.cost)
		return _a;
	else
		return _b;
}
