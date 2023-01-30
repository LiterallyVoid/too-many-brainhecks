#include <stdio.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>

static char *start;
static char *code;

// Registers:
// BL: current value of cell
// RBP: pointer to current cell

void writeU8(uint8_t i) {
	code[0] = i;
	code++;
}

void writeU16(uint16_t i) {
	writeU8(i);
	writeU8(i >> 8);
}

void writeU32(uint32_t i) {
	writeU16(i);
	writeU16(i >> 16);
}

static void writeOpDigitReg(uint8_t op, int digit, int reg) {
	writeU8(op);
	writeU8(0xC0 | (digit << 3) | (reg << 0));
}

static void writeInc(void) {
	// INC: FE /0 reg/mem8
	writeOpDigitReg(0xFE, 0, 3);
}

static void writeDec(void) {
	// DEC: FE /1 reg/mem8
	writeOpDigitReg(0xFE, 1, 3);
}

static void writeMovBlRbp(uint8_t opcode) {
	int reg = 3; // BL
	int regmem = 5; // RBP

	//writeU8(0x40); // Empty REX to allow RSP/RBP/RSI/RDI addressing
	writeU8(opcode);
	writeU8(0x80 | (reg << 3) | (regmem << 0));
	writeU32(0);
}

static void writeStore(void) {
	// AL -> BYTE PTR [RBP]
	writeMovBlRbp(0x88);
}

static void writeLoad(void) {
	// BYTE PTR [RBP] -> AL
	writeMovBlRbp(0x8A);
}

static void writeRbpMod(uint8_t opcode, uint8_t digit) {
	int reg = 5; // RBP

	writeU8(0x40 | (8)); // REX .X
	writeU8(opcode);
	writeU8(0xC0 | (digit << 3) | (reg << 0));
}

static void writeMovL(void) {
	writeStore();
	writeRbpMod(0xFF, 1);
	writeLoad();
}

static void writeMovR(void) {
	writeStore();
	writeRbpMod(0xFF, 0);
	writeLoad();
}

static void writeMovImm(uint8_t reg, uint32_t value) {
	int rex_r = (reg & 0x8) ? 4 : 0;
	writeU8(0x40 | rex_r);
	writeU8(0xB8 + (reg & 0x7));
	writeU32(value);
}

static void writeSyscall(uint32_t no) {
	writeMovImm(0b000, no); // RAX = no
	// syscall: 0F 05
	writeU8(0x0F);
	writeU8(0x05);
}

static void writeSyscallImm(uint32_t no, uint32_t a, uint32_t b, uint32_t c) {
	writeMovImm(0b111, a); // RDI = a
	writeMovImm(0b110, b); // RSI = b
	writeMovImm(0b010, c); // RDX = c

	writeSyscall(no);
}

static void writeEmit(void) {
	writeStore();

	// syscall arg0: fd
	writeMovImm(0b111, 0 /* stdout */);

	// syscall arg1: ptr
	// MOV RSI, RBP
	writeU8(0x40 | 8); // REX.W
	writeOpDigitReg(0x8B, 0b110, 0b101);

	// syscall arg2: count (1)
	writeMovImm(0b010, 1);

	writeSyscall(1);
}

static void writeRead(void) {
	writeU8(0x90);
}

static void patchU32(char *code_ptr, uint32_t value) {
	char *code_sv = code;
	code = code_ptr;
	writeU32(value);
	code = code_sv;
}

int main(int argc, char **argv) {
	char *source = NULL;

	FILE *sfp = fopen(argv[1], "rb");
	if (!sfp) return 3;

	source = malloc(8 * 1024 * 1024);

	ssize_t amt = fread(source, 1, 8 * 1024 * 1024, sfp);
	if (amt < 0) return 9;
	source[amt] = 0;

	fclose(sfp);

	size_t codesz = 8 * 1024 * 1024;

	// 8 megabytes oughta be enough for anybody!
	start = mmap(NULL, codesz, PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	code = start;

	char *stack[1024] = { NULL };
	int stack_i = 0;

	int DATA_SIZE = 0x10000;

	// RAX = DATA_SIZE
	writeMovImm(0, DATA_SIZE / 4);

	// loop:

	// SUB RSP, 4
	writeU8(0x40 | 8);
	writeOpDigitReg(0x81, 5, 0b100);
	writeU32(4);

	// MOV DWORD PTR [RSP+0], 0
	writeU8(0xC7);
	writeU8(0x00 | (0b100));
	writeU8((0b100 << 3) | 0b100);
	writeU32(0);

	// SUB RAX, 1
	writeU8(0x40 | 8);
	writeOpDigitReg(0x81, 5, 0b000);
	writeU32(1);

	// JNZ loop
	writeU8(0x0F);
	writeU8(0x85);
	writeU32(~(7 + 7 + 7 + 6) + 1);

	// MOV RBP, RSP
	writeU8(0x40 | 8); // REX.W
	writeOpDigitReg(0x8B, 0b101, 0b100);

	writeLoad();

	for (; *source; source++) {
		switch (*source) {
		case '+': writeInc(); break;
		case '-': writeDec(); break;
		case '<': writeMovL(); break;
		case '>': writeMovR(); break;
		case '[': {
			// Unconditionally jump to the end bracket
			writeU8(0xE9);
			stack[stack_i++] = code;
			writeU32(0);
			break;
		};

		case ']': {
			char *ptr = stack[--stack_i];
			patchU32(ptr, code - (ptr + 4));
			// CMP reg/mem8, imm8 | 80 /7 ib
			writeOpDigitReg(0x80, 7, 3);
			writeU8(0x00);

			// JNZ: 0F 85 cd
			writeU8(0x0F);
			writeU8(0x85);

			// Jump to the end of the `[` instruction
			uint32_t offset = (code + 4) - (ptr + 4);

			writeU32(~offset + 1);

			break;
		};
		case '.': writeEmit(); break;
		case ',': writeRead(); break;
		}
	}

	writeSyscallImm(60, 0, 0, 0);

	FILE *fp = fopen("code.bin", "wb");
	fwrite(start, 1, code - start, fp);
	fclose(fp);

	//return 43;

	int err = mprotect(start, codesz, PROT_READ | PROT_EXEC);
	if (err < 0) {
		perror("mprotect");
	}

	((void(*)(void))start)();

	return 42;
}
