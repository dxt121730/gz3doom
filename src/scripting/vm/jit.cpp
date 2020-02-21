
#include "jit.h"
#include "i_system.h"

// To do: get cmake to define these..
#define ASMJIT_BUILD_EMBED
#define ASMJIT_STATIC

#include <asmjit/asmjit.h>
#include <asmjit/x86.h>

class AsmJitException : public std::exception
{
public:
	AsmJitException(asmjit::Error error, const char *message) noexcept : error(error), message(message)
	{
	}

	const char* what() const noexcept override
	{
		return message.c_str();
	}

	asmjit::Error error;
	std::string message;
};

class ThrowingErrorHandler : public asmjit::ErrorHandler
{
public:
	bool handleError(asmjit::Error err, const char *message, asmjit::CodeEmitter *origin) override
	{
		throw AsmJitException(err, message);
	}
};

static asmjit::JitRuntime jit;

#define A				(pc[0].a)
#define B				(pc[0].b)
#define C				(pc[0].c)
#define Cs				(pc[0].cs)
#define BC				(pc[0].i16u)
#define BCs				(pc[0].i16)
#define ABCs			(pc[0].i24)
#define JMPOFS(x)		((x)->i24)
#define KC				(konstd[C])
#define RC				(reg.d[C])
#define PA				(reg.a[A])
#define PB				(reg.a[B])

#define ASSERTD(x)		assert((unsigned)(x) < sfunc->NumRegD)
#define ASSERTF(x)		assert((unsigned)(x) < sfunc->NumRegF)
#define ASSERTA(x)		assert((unsigned)(x) < sfunc->NumRegA)
#define ASSERTS(x)		assert((unsigned)(x) < sfunc->NumRegS)
#define ASSERTKD(x)		assert(sfunc != NULL && (unsigned)(x) < sfunc->NumKonstD)
#define ASSERTKF(x)		assert(sfunc != NULL && (unsigned)(x) < sfunc->NumKonstF)
#define ASSERTKA(x)		assert(sfunc != NULL && (unsigned)(x) < sfunc->NumKonstA)
#define ASSERTKS(x)		assert(sfunc != NULL && (unsigned)(x) < sfunc->NumKonstS)

static bool CanJit(VMScriptFunction *sfunc)
{
	int size = sfunc->CodeSize;
	for (int i = 0; i < size; i++)
	{
		const VMOP *pc = sfunc->Code + i;
		VM_UBYTE op = pc->op;
		int a = pc->a;

		switch (op)
		{
		default:
			return false;
		case OP_NOP:
		case OP_LI:
		case OP_LK:
		//case OP_LKF:
		//case OP_LKS:
		//case OP_LKP:
		case OP_LK_R:
		//case OP_LKF_R:
		//case OP_LKS_R:
		//case OP_LKP_R:
		case OP_MOVE:
		//case OP_MOVEF:
		//case OP_MOVES:
		//case OP_MOVEA:
		//case OP_MOVEV2:
		//case OP_MOVEV3:
			break;
		case OP_RET:
			if (B != REGT_NIL)
			{
				int regtype = B;
				int regnum = C;
				switch (regtype & REGT_TYPE)
				{
				case REGT_FLOAT:
				case REGT_STRING:
				case REGT_POINTER:
					return false;
				}
			}
			break;
		case OP_RETI:
		case OP_SLL_RR:
		case OP_SLL_RI:
		case OP_SLL_KR:
		case OP_SRL_RR:
		case OP_SRL_RI:
		case OP_SRL_KR:
		case OP_SRA_RR:
		case OP_SRA_RI:
		case OP_SRA_KR:
		case OP_ADD_RR:
		case OP_ADD_RK:
		case OP_ADDI:
		case OP_SUB_RR:
		case OP_SUB_RK:
		case OP_SUB_KR:
		case OP_MUL_RR:
		case OP_MUL_RK:
		case OP_DIV_RR:
		case OP_DIV_RK:
		case OP_DIV_KR:
		case OP_DIVU_RR:
		case OP_DIVU_RK:
		case OP_DIVU_KR:
		//case OP_MOD_RR:
		//case OP_MOD_RK:
		//case OP_MOD_KR:
		//case OP_MODU_RR:
		//case OP_MODU_RK:
		//case OP_MODU_KR:
		case OP_AND_RR:
		case OP_AND_RK:
		case OP_OR_RR:
		case OP_OR_RK:
		case OP_XOR_RR:
		case OP_XOR_RK:
		//case OP_MIN_RR:
		//case OP_MIN_RK:
		//case OP_MAX_RR:
		//case OP_MAX_RK:
		//case OP_ABS:
		case OP_NEG:
		case OP_NOT:
			break;
		}
	}
	return true;
}

JitFuncPtr JitCompile(VMScriptFunction *sfunc)
{
#if 0 // For debugging
	if (strcmp(sfunc->Name.GetChars(), "EmptyFunction") != 0 && !CanJit(sfunc))
		return nullptr;
#else
	if (!CanJit(sfunc))
		return nullptr;
#endif

	using namespace asmjit;
	try
	{
		ThrowingErrorHandler errorHandler;
		//FileLogger logger(stdout);
		CodeHolder code;
		code.init(jit.getCodeInfo());
		code.setErrorHandler(&errorHandler);
		//code.setLogger(&logger);

		X86Compiler cc(&code);

		X86Gp stack = cc.newIntPtr("stack"); // VMFrameStack *stack
		X86Gp ret = cc.newIntPtr("ret"); // VMReturn *ret
		X86Gp numret = cc.newInt32("numret"); // int numret

		cc.addFunc(FuncSignature3<int, void/*VMFrameStack*/ *, void/*VMReturn*/ *, int>());
		cc.setArg(0, stack);
		cc.setArg(1, ret);
		cc.setArg(2, numret);

		const int *konstd = sfunc->KonstD;
		const double *konstf = sfunc->KonstF;
		const FString *konsts = sfunc->KonstS;
		const FVoidObj *konsta = sfunc->KonstA;

		TArray<X86Gp> regD(sfunc->NumRegD, true);
		TArray<X86Xmm> regF(sfunc->NumRegF, true);
		//TArray<X86Gp> regA(sfunc->NumRegA, true);
		//TArray<X86Gp> regS(sfunc->NumRegS, true);

		for (int i = 0; i < sfunc->NumRegD; i++) regD[i] = cc.newInt32();
		for (int i = 0; i < sfunc->NumRegF; i++) regF[i] = cc.newXmm();
		//for (int i = 0; i < sfunc->NumRegA; i++) regA[i] = cc.newIntPtr();
		//for (int i = 0; i < sfunc->NumRegS; i++) regS[i] = cc.newGpd();

		int size = sfunc->CodeSize;
		for (int i = 0; i < size; i++)
		{
			const VMOP *pc = sfunc->Code + i;
			VM_UBYTE op = pc->op;
			int a = pc->a;
			int b;// , c;

			switch (op)
			{
			default:
				break;

			case OP_NOP: // no operation
				cc.nop();
				break;

			// Load constants.
			case OP_LI: // load immediate signed 16-bit constant
				cc.mov(regD[a], BCs);
				break;
			case OP_LK: // load integer constant
				cc.mov(regD[a], konstd[BC]);
				break;
			case OP_LKF: // load float constant
				cc.movsd(regF[a], x86::ptr((ptrdiff_t)&konstf[BC]));
				break;
			case OP_LKS: // load string constant
				//cc.mov(regS[a], konsts[BC]);
				break;
			case OP_LKP: // load pointer constant
				//cc.mov(regA[a], konsta[BC].v);
				break;
			case OP_LK_R: // load integer constant indexed
				cc.mov(regD[a], x86::ptr((ptrdiff_t)konstd, regD[B], 2, C * 4));
				break;
			case OP_LKF_R: // load float constant indexed
				cc.movsd(regF[a], x86::ptr((ptrdiff_t)konstf, regD[B], 3, C * 8));
				break;
			case OP_LKS_R: // load string constant indexed
				//cc.mov(regS[a], konsts[regD[B] + C]);
				break;
			case OP_LKP_R: // load pointer constant indexed
				//cc.mov(b, regD[B] + C);
				//cc.mov(regA[a], konsta[b].v);
				break;
			case OP_LFP: // load frame pointer
			case OP_META: // load a class's meta data address
			case OP_CLSS: // load a class's descriptor address
				break;

			// Load from memory. rA = *(rB + rkC)
			case OP_LB: // load byte
			case OP_LB_R:
			case OP_LH: // load halfword
			case OP_LH_R:
			case OP_LW: // load word
			case OP_LW_R:
			case OP_LBU: // load byte unsigned
			case OP_LBU_R:
			case OP_LHU: // load halfword unsigned
			case OP_LHU_R:
			case OP_LSP: // load single-precision fp
			case OP_LSP_R:
			case OP_LDP: // load double-precision fp
			case OP_LDP_R:
			case OP_LS: // load string
			case OP_LS_R:
			case OP_LO: // load object
			case OP_LO_R:
			case OP_LP: // load pointer
			case OP_LP_R:
			case OP_LV2: // load vector2
			case OP_LV2_R:
			case OP_LV3: // load vector3
			case OP_LV3_R:
			case OP_LCS: // load string from char ptr.
			case OP_LCS_R:
			case OP_LBIT: // rA = !!(*rB & C)  -- *rB is a byte
				break;

			// Store instructions. *(rA + rkC) = rB
			case OP_SB: // store byte
			case OP_SB_R:
			case OP_SH: // store halfword
			case OP_SH_R:
			case OP_SW: // store word
			case OP_SW_R:
			case OP_SSP: // store single-precision fp
			case OP_SSP_R:
			case OP_SDP: // store double-precision fp
			case OP_SDP_R:
			case OP_SS: // store string
			case OP_SS_R:
			case OP_SO: // store object pointer with write barrier (only needed for non thinkers and non types)
			case OP_SO_R:
			case OP_SP: // store pointer
			case OP_SP_R:
			case OP_SV2: // store vector2
			case OP_SV2_R:
			case OP_SV3: // store vector3
			case OP_SV3_R:
			case OP_SBIT: // *rA |= C if rB is true, *rA &= ~C otherwise
				break;

			// Move instructions.
			case OP_MOVE: // dA = dB
				cc.mov(regD[a], regD[B]);
				break;
			case OP_MOVEF: // fA = fB
				cc.movsd(regF[a], regF[B]);
				break;
			case OP_MOVES: // sA = sB
			case OP_MOVEA: // aA = aB
				break;
			case OP_MOVEV2: // fA = fB (2 elements)
				b = B;
				cc.movsd(regF[a], regF[b]);
				cc.movsd(regF[a + 1], regF[b + 1]);
				break;
			case OP_MOVEV3: // fA = fB (3 elements)
				b = B;
				cc.movsd(regF[a], regF[b]);
				cc.movsd(regF[a + 1], regF[b + 1]);
				cc.movsd(regF[a + 2], regF[b + 2]);
				break;
			case OP_CAST: // xA = xB, conversion specified by C
			case OP_CASTB: // xA = !!xB, type specified by C
			case OP_DYNCAST_R: // aA = dyn_cast<aC>(aB);
			case OP_DYNCAST_K: // aA = dyn_cast<aKC>(aB);
			case OP_DYNCASTC_R: // aA = dyn_cast<aC>(aB); for class types
			case OP_DYNCASTC_K: // aA = dyn_cast<aKC>(aB);
				break;

			// Control flow.
			case OP_TEST: // if (dA != BC) then pc++
			case OP_TESTN: // if (dA != -BC) then pc++
			case OP_JMP: // pc += ABC		-- The ABC fields contain a signed 24-bit offset.
			case OP_IJMP: // pc += dA + BC	-- BC is a signed offset. The target instruction must be a JMP.
			case OP_PARAM: // push parameter encoded in BC for function call (B=regtype, C=regnum)
			case OP_PARAMI: // push immediate, signed integer for function call
			case OP_CALL: // Call function pkA with parameter count B and expected result count C
			case OP_CALL_K:
			case OP_VTBL: // dereferences a virtual method table.
			case OP_SCOPE: // Scope check at runtime.
			case OP_TAIL: // Call+Ret in a single instruction
			case OP_TAIL_K:
			case OP_RESULT: // Result should go in register encoded in BC (in caller, after CALL)
			case OP_RET: // Copy value from register encoded in BC to return value A, possibly returning
				if (B == REGT_NIL)
				{
					X86Gp vReg = cc.newInt32();
					cc.mov(vReg, 0);
					cc.ret(vReg);
				}
				else
				{
					int retnum = a & ~RET_FINAL;

					X86Gp reg_retnum = cc.newInt32();
					X86Gp location = cc.newIntPtr();
					Label L_endif = cc.newLabel();

					cc.mov(reg_retnum, retnum);
					cc.test(reg_retnum, numret); // Operand size mismatch: test eax, rcx
					cc.jg(L_endif);

					cc.mov(location, x86::ptr(ret, retnum * sizeof(VMReturn)));

					int regtype = B;
					int regnum = C;
					switch (regtype & REGT_TYPE)
					{
					case REGT_INT:
						if (regtype & REGT_KONST)
							cc.mov(x86::dword_ptr(location), konstd[regnum]);
						else
							cc.mov(x86::dword_ptr(location), regD[regnum]);
						break;
					case REGT_FLOAT:
						if (regtype & REGT_KONST)
						{
							if (regtype & REGT_MULTIREG3)
							{
								cc.mov(x86::qword_ptr(location), (int64_t)(ptrdiff_t)&konstf[regnum]);
								cc.mov(x86::qword_ptr(location, 8), (int64_t)(ptrdiff_t)&konstf[regnum + 1]);
								cc.mov(x86::qword_ptr(location, 16), (int64_t)(ptrdiff_t)&konstf[regnum + 2]);
							}
							else if (regtype & REGT_MULTIREG2)
							{
								cc.mov(x86::qword_ptr(location), (int64_t)(ptrdiff_t)&konstf[regnum]);
								cc.mov(x86::qword_ptr(location, 8), (int64_t)(ptrdiff_t)&konstf[regnum + 1]);
							}
							else
							{
								cc.mov(x86::qword_ptr(location), (int64_t)(ptrdiff_t)&konstf[regnum]);
							}
						}
						else
						{
							if (regtype & REGT_MULTIREG3)
							{
								cc.movsd(x86::qword_ptr(location), regF[regnum]);
								cc.movsd(x86::qword_ptr(location, 8), regF[regnum + 1]);
								cc.movsd(x86::qword_ptr(location, 16), regF[regnum + 2]);
							}
							else if (regtype & REGT_MULTIREG2)
							{
								cc.movsd(x86::qword_ptr(location), regF[regnum]);
								cc.movsd(x86::qword_ptr(location, 8), regF[regnum + 1]);
							}
							else
							{
								cc.movsd(x86::qword_ptr(location), regF[regnum]);
							}
						}
						break;
					case REGT_STRING:
					case REGT_POINTER:
						break;
					}

					if (a & RET_FINAL)
					{
						cc.add(reg_retnum, 1);
						cc.ret(reg_retnum);
					}

					cc.bind(L_endif);
					if (a & RET_FINAL)
						cc.ret(numret);
				}
				break;
			case OP_RETI: // Copy immediate from BC to return value A, possibly returning
				{
					int retnum = a & ~RET_FINAL;

					X86Gp reg_retnum = cc.newInt32();
					X86Gp location = cc.newIntPtr();
					Label L_endif = cc.newLabel();

					cc.mov(reg_retnum, retnum);
					cc.test(reg_retnum, numret);
					cc.jg(L_endif);

					cc.mov(location, x86::ptr(ret, retnum * sizeof(VMReturn)));
					cc.mov(x86::dword_ptr(location), BCs);

					if (a & RET_FINAL)
					{
						cc.add(reg_retnum, 1);
						cc.ret(reg_retnum);
					}

					cc.bind(L_endif);
					if (a & RET_FINAL)
						cc.ret(numret);
				}
				break;

			case OP_NEW:
			case OP_NEW_K:
			case OP_THROW: // A == 0: Throw exception object pB, A == 1: Throw exception object pkB, A >= 2: Throw VM exception of type BC
			case OP_BOUND: // if rA < 0 or rA >= BC, throw exception
			case OP_BOUND_K: // if rA < 0 or rA >= const[BC], throw exception
			case OP_BOUND_R: // if rA < 0 or rA >= rB, throw exception
				break;

			// String instructions.
			case OP_CONCAT: // sA = sB..sC
			case OP_LENS: // dA = sB.Length
			case OP_CMPS: // if ((skB op skC) != (A & 1)) then pc++
				break;

			// Integer math.
			case OP_SLL_RR: // dA = dkB << diC
				cc.mov(regD[a], regD[B]);
				cc.shl(regD[a], regD[C]);
				break;
			case OP_SLL_RI:
				cc.mov(regD[a], regD[B]);
				cc.shl(regD[a], C);
				break;
			case OP_SLL_KR:
				cc.mov(regD[a], konstd[B]);
				cc.shl(regD[a], C);
				break;
			case OP_SRL_RR: // dA = dkB >> diC  -- unsigned
				cc.mov(regD[a], regD[B]);
				cc.shr(regD[a], regD[C]);
				break;
			case OP_SRL_RI:
				cc.mov(regD[a], regD[B]);
				cc.shr(regD[a], C);
				break;
			case OP_SRL_KR:
				cc.mov(regD[a], konstd[B]);
				cc.shr(regD[a], C);
				break;
			case OP_SRA_RR: // dA = dkB >> diC  -- signed
				cc.mov(regD[a], regD[B]);
				cc.sar(regD[a], regD[C]);
				break;
			case OP_SRA_RI:
				cc.mov(regD[a], regD[B]);
				cc.sar(regD[a], C);
				break;
			case OP_SRA_KR:
				cc.mov(regD[a], konstd[B]);
				cc.sar(regD[a], regD[C]);
				break;
			case OP_ADD_RR: // dA = dB + dkC
				cc.mov(regD[a], regD[B]);
				cc.add(regD[a], regD[C]);
				break;
			case OP_ADD_RK:
				cc.mov(regD[a], regD[B]);
				cc.add(regD[a], konstd[C]);
				break;
			case OP_ADDI: // dA = dB + C		-- C is a signed 8-bit constant
				cc.mov(regD[a], regD[B]);
				cc.add(regD[a], Cs);
				break;
			case OP_SUB_RR: // dA = dkB - dkC
				cc.mov(regD[a], regD[B]);
				cc.sub(regD[a], regD[C]);
				break;
			case OP_SUB_RK:
				cc.mov(regD[a], regD[B]);
				cc.sub(regD[a], konstd[C]);
				break;
			case OP_SUB_KR:
				cc.mov(regD[a], konstd[B]);
				cc.sub(regD[a], regD[C]);
				break;
			case OP_MUL_RR: // dA = dB * dkC
				cc.mov(regD[a], regD[B]);
				cc.mul(regD[a], regD[C]);
				break;
			case OP_MUL_RK:
				cc.mov(regD[a], regD[B]);
				cc.mul(regD[a], konstd[C]);
				break;
			case OP_DIV_RR: // dA = dkB / dkC (signed)
				// To do: ThrowAbortException(X_DIVISION_BY_ZERO, nullptr);
				cc.mov(regD[a], regD[B]);
				cc.idiv(regD[a], regD[C]);
				break;
			case OP_DIV_RK:
				// To do: ThrowAbortException(X_DIVISION_BY_ZERO, nullptr);
				cc.mov(regD[a], regD[B]);
				cc.div(regD[a], konstd[C]);
				break;
			case OP_DIV_KR:
				// To do: ThrowAbortException(X_DIVISION_BY_ZERO, nullptr);
				cc.mov(regD[a], konstd[B]);
				cc.idiv(regD[a], regD[C]);
				break;
			case OP_DIVU_RR: // dA = dkB / dkC (unsigned)
				 // To do: ThrowAbortException(X_DIVISION_BY_ZERO, nullptr);
				cc.mov(regD[a], regD[B]);
				cc.div(regD[a], regD[C]);
				break;
			case OP_DIVU_RK:
				// To do: ThrowAbortException(X_DIVISION_BY_ZERO, nullptr);
				cc.mov(regD[a], regD[B]);
				cc.div(regD[a], konstd[C]);
				break;
			case OP_DIVU_KR:
				// To do: ThrowAbortException(X_DIVISION_BY_ZERO, nullptr);
				cc.mov(regD[a], konstd[B]);
				cc.div(regD[a], regD[C]);
				break;
			case OP_MOD_RR: // dA = dkB % dkC (signed)
			case OP_MOD_RK:
			case OP_MOD_KR:
			case OP_MODU_RR: // dA = dkB % dkC (unsigned)
			case OP_MODU_RK:
			case OP_MODU_KR:
				break;
			case OP_AND_RR: // dA = dB & dkC
				cc.mov(regD[a], regD[B]);
				cc.and_(regD[a], regD[C]);
				break;
			case OP_AND_RK:
				cc.mov(regD[a], regD[B]);
				cc.and_(regD[a], konstd[C]);
				break;
			case OP_OR_RR: // dA = dB | dkC
				cc.mov(regD[a], regD[B]);
				cc.or_(regD[a], regD[C]);
				break;
			case OP_OR_RK:
				cc.mov(regD[a], regD[B]);
				cc.or_(regD[a], konstd[C]);
				break;
			case OP_XOR_RR: // dA = dB ^ dkC
				cc.mov(regD[a], regD[B]);
				cc.xor_(regD[a], regD[C]);
				break;
			case OP_XOR_RK:
				cc.mov(regD[a], regD[B]);
				cc.xor_(regD[a], konstd[C]);
				break;
			case OP_MIN_RR: // dA = min(dB,dkC)
			case OP_MIN_RK:
			case OP_MAX_RR: // dA = max(dB,dkC)
			case OP_MAX_RK:
			case OP_ABS: // dA = abs(dB)
				break;
			case OP_NEG: // dA = -dB
				cc.xor_(regD[a], regD[a]);
				cc.sub(regD[a], regD[B]);
				break;
			case OP_NOT: // dA = ~dB
				cc.mov(regD[a], regD[B]);
				cc.not_(regD[a]);
				break;
			case OP_EQ_R: // if ((dB == dkC) != A) then pc++
			case OP_EQ_K:
			case OP_LT_RR: // if ((dkB < dkC) != A) then pc++
			case OP_LT_RK:
			case OP_LT_KR:
			case OP_LE_RR: // if ((dkB <= dkC) != A) then pc++
			case OP_LE_RK:
			case OP_LE_KR:
			case OP_LTU_RR: // if ((dkB < dkC) != A) then pc++		-- unsigned
			case OP_LTU_RK:
			case OP_LTU_KR:
			case OP_LEU_RR: // if ((dkB <= dkC) != A) then pc++		-- unsigned
			case OP_LEU_RK:
			case OP_LEU_KR:
				break;

			// Double-precision floating point math.
			case OP_ADDF_RR: // fA = fB + fkC
			case OP_ADDF_RK:
			case OP_SUBF_RR: // fA = fkB - fkC
			case OP_SUBF_RK:
			case OP_SUBF_KR:
			case OP_MULF_RR: // fA = fB * fkC
			case OP_MULF_RK:
			case OP_DIVF_RR: // fA = fkB / fkC
			case OP_DIVF_RK:
			case OP_DIVF_KR:
			case OP_MODF_RR: // fA = fkB % fkC
			case OP_MODF_RK:
			case OP_MODF_KR:
			case OP_POWF_RR: // fA = fkB ** fkC
			case OP_POWF_RK:
			case OP_POWF_KR:
			case OP_MINF_RR: // fA = min(fB),fkC)
			case OP_MINF_RK:
			case OP_MAXF_RR: // fA = max(fB),fkC)
			case OP_MAXF_RK:
			case OP_ATAN2: // fA = atan2(fB,fC), result is in degrees
			case OP_FLOP: // fA = f(fB), where function is selected by C
			case OP_EQF_R: // if ((fB == fkC) != (A & 1)) then pc++
			case OP_EQF_K:
			case OP_LTF_RR: // if ((fkB < fkC) != (A & 1)) then pc++
			case OP_LTF_RK:
			case OP_LTF_KR:
			case OP_LEF_RR: // if ((fkb <= fkC) != (A & 1)) then pc++
			case OP_LEF_RK:
			case OP_LEF_KR:
				break;

			// Vector math. (2D)
			case OP_NEGV2: // vA = -vB
			case OP_ADDV2_RR: // vA = vB + vkC
			case OP_SUBV2_RR: // vA = vkB - vkC
			case OP_DOTV2_RR: // va = vB dot vkC
			case OP_MULVF2_RR: // vA = vkB * fkC
			case OP_MULVF2_RK:
			case OP_DIVVF2_RR: // vA = vkB / fkC
			case OP_DIVVF2_RK:
			case OP_LENV2: // fA = vB.Length
			case OP_EQV2_R: // if ((vB == vkC) != A) then pc++ (inexact if A & 32)
			case OP_EQV2_K: // this will never be used.
				break;

			// Vector math. (3D)
			case OP_NEGV3: // vA = -vB
			case OP_ADDV3_RR: // vA = vB + vkC
			case OP_SUBV3_RR: // vA = vkB - vkC
			case OP_DOTV3_RR: // va = vB dot vkC
			case OP_CROSSV_RR: // vA = vkB cross vkC
			case OP_MULVF3_RR: // vA = vkB * fkC
			case OP_MULVF3_RK:
			case OP_DIVVF3_RR: // vA = vkB / fkC
			case OP_DIVVF3_RK:
			case OP_LENV3: // fA = vB.Length
			case OP_EQV3_R: // if ((vB == vkC) != A) then pc++ (inexact if A & 32)
			case OP_EQV3_K: // this will never be used.
				break;

			// Pointer math.
			case OP_ADDA_RR: // pA = pB + dkC
			case OP_ADDA_RK:
			case OP_SUBA: // dA = pB - pC
			case OP_EQA_R: // if ((pB == pkC) != A) then pc++
			case OP_EQA_K:
				break;
			}
		}

		cc.endFunc();
		cc.finalize();

		JitFuncPtr fn = nullptr;
		Error err = jit.add(&fn, &code);
		if (err)
			I_FatalError("JitRuntime::add failed: %d", err);
		return fn;
	}
	catch (const std::exception &e)
	{
		I_FatalError("Unexpected JIT error: %s", e.what());
		return nullptr;
	}
}
