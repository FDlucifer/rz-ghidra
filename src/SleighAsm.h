// SPDX-FileCopyrightText: 2020-2021 Florian Märkl <info@florianmaerkl.de>
// SPDX-FileCopyrightText: 2020 FXTi <zjxiang1998@gmail.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef RZ_GHIDRA_SLEIGHASM_H
#define RZ_GHIDRA_SLEIGHASM_H

#include <string>
#include <vector>
#include <rz_core.h>
#include <unordered_map>

#ifdef LoadImage
#undef LoadImage
#endif
#ifdef CONST
#undef CONST
#endif

#include "architecture.hh"
#include "sleigh_arch.hh"
#include "SleighInstruction.h"

class AsmLoadImage : public LoadImage
{
private:
	std::unique_ptr<RzBuffer, decltype(&rz_buf_free)> buf;

public:
	AsmLoadImage();
	void loadFill(uint1 *ptr, int4 size, const Address &addr) override;
	string getArchType(void) const override { return "rizin"; }
	void adjustVma(long adjust) override { throw LowlevelError("Cannot adjust rizin virtual memory"); }

	void resetBuffer(ut64 offset, const ut8 *data, size_t size);
};

class SleighAsm;
class AssemblySlg : public AssemblyEmit
{
private:
	SleighAsm *sasm = nullptr;

public:
	char *str = nullptr;

	AssemblySlg(SleighAsm *s): sasm(s) {}

	void dump(const Address &addr, const string &mnem, const string &body) override;

	~AssemblySlg()
	{
		if(str)
			rz_mem_free(str);
	}
};

struct PcodeOperand
{
	PcodeOperand(uintb offset, uint4 size): type(RAM), offset(offset), size(size) {}
	PcodeOperand(uintb number): type(CONST), number(number), size(0) {}
	PcodeOperand(const std::string &name, uint4 size): type(REGISTER), name(name), size(size) {}
	virtual ~PcodeOperand()
	{
		if(type == REGISTER)
			name.~string();
	}

	union
	{
		std::string name;
		uintb offset;
		uintb number;
	};
	uint4 size;

	enum
	{
		REGISTER,
		RAM,
		CONST,
		UNIQUE
	} type;

	PcodeOperand(const PcodeOperand &rhs)
	{
		type = rhs.type;
		size = rhs.size;

		switch(type)
		{
			case REGISTER: name = rhs.name; break;
			case UNIQUE: /* Same as RAM */
			case RAM: offset = rhs.offset; break;
			case CONST: number = rhs.number; break;
			default: throw LowlevelError("Unexpected type of PcodeOperand found in operator==.");
		}
	}

	bool operator==(const PcodeOperand &rhs) const
	{
		if(type != rhs.type)
			return false;

		switch(type)
		{
			case REGISTER: return name == rhs.name;
			case UNIQUE: /* Same as RAM */
			case RAM: return offset == rhs.offset && size == rhs.size;
			case CONST: return number == rhs.number;
			default: throw LowlevelError("Unexpected type of PcodeOperand found in operator==.");
		}
	}

	bool is_unique() const { return type == UNIQUE; }

	bool is_const() const { return type == CONST; }

	bool is_ram() const { return type == RAM; }

	bool is_reg() const { return type == REGISTER; }
};

ostream &operator<<(ostream &s, const PcodeOperand &arg);

typedef OpCode PcodeOpType;

struct Pcodeop
{
	PcodeOpType type;

	PcodeOperand *output = nullptr;
	PcodeOperand *input0 = nullptr;
	PcodeOperand *input1 = nullptr;
	/* input2 for STORE will use output to save memory space */

	Pcodeop(PcodeOpType opc, PcodeOperand *in0, PcodeOperand *in1, PcodeOperand *out):
	    type(opc), input0(in0), input1(in1), output(out)
	{
	}

	void fini()
	{
		if(output)
			delete output;
		if(input0)
			delete input0;
		if(input1)
			delete input1;
	}
};

ostream &operator<<(ostream &s, const Pcodeop &op);

struct UniquePcodeOperand: public PcodeOperand
{
	const Pcodeop *def = nullptr;
	UniquePcodeOperand(const PcodeOperand *from): PcodeOperand(*from) {}
	~UniquePcodeOperand() = default;
};

class PcodeSlg : public PcodeEmit
{
private:
	SleighAsm *sanalysis = nullptr;

	PcodeOperand *parse_vardata(VarnodeData &data);

public:
	std::vector<Pcodeop> pcodes;

	PcodeSlg(SleighAsm *s): sanalysis(s) {}

	void dump(const Address &addr, OpCode opc, VarnodeData *outvar, VarnodeData *vars,
	          int4 isize) override
	{
		PcodeOperand *out = nullptr, *in0 = nullptr, *in1 = nullptr;

		if(opc == CPUI_CALLOTHER)
			isize = isize > 2? 2: isize;

		switch(isize)
		{
			case 3: out = parse_vardata(vars[2]); // Only for STORE
			case 2: in1 = parse_vardata(vars[1]);
			case 1: in0 = parse_vardata(vars[0]);
			case 0: break;
			default: throw LowlevelError("Unexpexted isize in PcodeSlg::dump()");
		}

		if(outvar)
			out = parse_vardata(*outvar);

		pcodes.push_back(Pcodeop(opc, in0, in1, out));
	}

	~PcodeSlg()
	{
		while(!pcodes.empty())
		{
			pcodes.back().fini();
			pcodes.pop_back();
		}
	}
};

struct RizinReg
{
	std::string name;
	ut64 size;
	ut64 offset;
};

class RizinSleigh;

class SleighAsm
{
private:
	AsmLoadImage loader;
	ContextInternal context;
	DocumentStorage docstorage;
	FileManage specpaths;
	std::vector<LanguageDescription> description;
	int languageindex;

	void initInner(std::string sleigh_id);
	void initRegMapping(void);
	std::string getSleighHome(RzConfig *cfg);
	void collectSpecfiles(void);
	void scanSleigh(const string &rootpath);
	void resolveArch(const string &archid);
	void buildSpecfile(DocumentStorage &store);
	void parseProcConfig(DocumentStorage &store);
	void parseCompConfig(DocumentStorage &store);
	void loadLanguageDescription(const string &specfile);
	void resetBuffer(ut64 offset, const ut8 *buf, size_t size);

public:
	RizinSleigh trans;
	std::string sleigh_id;
	int alignment = 1;
	std::string pc_name;
	std::string sp_name;
	std::vector<std::string> arg_names; // default ABI's function args
	std::vector<std::string> ret_names; // default ABI's function retvals
	std::unordered_map<std::string, std::string> reg_group;
	// To satisfy rizin's rule: reg name has to be lowercase.
	std::unordered_map<std::string, std::string> reg_mapping;
	SleighAsm(): trans(nullptr, nullptr) {}
	void init(const char *cpu, int bits, bool bigendian, RzConfig *cfg);
	int disassemble(RzAsmOp *op, ut64 offset, const ut8 *buf, size_t size);
	int genOpcode(PcodeSlg &pcode_slg, Address &addr, const ut8 *buf, size_t size);
	std::vector<RizinReg> getRegs(void);
	static RzConfig *getConfig(RzAsm *a);
	static RzConfig *getConfig(RzAnalysis *a);
};

#endif // RZ_GHIDRA_SLEIGHASM_H
