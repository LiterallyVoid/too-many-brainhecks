#include <stdio.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

enum OpKind {
	OP_NOP = 0,
	OP_END,

	OP_ADD_PTR,

	OP_ADD,

	OP_LABEL,
	OP_JMP,
	OP_BR_IFNZ,

	OP_READ,
	OP_WRITE,
};

struct OpCode {
	enum OpKind kind;
	union {
		uint8_t add; // OP_ADD
		int32_t mem_offset; // OP_LOAD, OP_STORE, OP_ADD_PTR
		uint32_t label; // OP_LABEL, OP_JMP, OP_BR_IFZ
		void *jump; // OP_JMP, OP_BR_IFNZ
	};
};

#define MAX_CODE_LEN 8 * 1024 * 1024

static struct OpCode *ops, *ops_into;

void swap(void) {
	struct OpCode *tmp = ops;
	ops = ops_into;
	ops_into = tmp;
}

void dumpOps(void) {
	for (size_t i = 0; ops[i].kind != OP_END; i++) {
		switch (ops[i].kind) {
		case OP_NOP: printf("\tNOP\n"); break;
		case OP_END: printf("\tEND\n"); break;
		case OP_ADD_PTR: printf("\tADD_PTR %d\n", ops[i].mem_offset); break;
		case OP_ADD: printf("\tADD %d\n", (int) ops[i].add); break;
		case OP_LABEL: printf("%u:\n", ops[i].label); break;
		case OP_JMP: printf("\tJMP %u\n", ops[i].label); break;
		case OP_BR_IFNZ: printf("\tJNZ %u\n", ops[i].label); break;
		case OP_READ: printf("\tREAD\n"); break;
		case OP_WRITE: printf("\tWRITE\n"); break;
		}
	}
}

void coalesce(void) {
	size_t i;
	size_t last = 0;
	for (i = 0; ops[i].kind != OP_END; i++) {
		ops_into[last] = ops[i];

		if (last > 0 && ops_into[last - 1].kind == ops[i].kind) {
			if (ops[i].kind == OP_ADD) {
				ops_into[last - 1].add += ops[i].add;
				continue;
			}

			if (ops[i].kind == OP_ADD_PTR) {
				ops_into[last - 1].mem_offset += ops[i].mem_offset;
				continue;
			}
		}

		last++;
	}

	ops_into[last] = ops[i];
}

void runCodegen(void) {
	printf(".intel_syntax noprefix\n");
	printf(".globl main\n");
	printf("main:\n");
	printf("\tmov RAX, 0x4000\n");
	printf("loop:\n");
	printf("\tsub RSP, 4\n");
	printf("\tmov DWORD PTR [RSP], 0\n");
	printf("\tsub RAX, 1\n");
	printf("\tjnz loop\n");
	printf("\tmov RBP, RSP\n");

	for (size_t i = 0; ops[i].kind != OP_END; i++) {
		switch (ops[i].kind) {
		case OP_NOP:
			printf("\tnop\n");
			break;
		case OP_END:
			break;
		case OP_ADD_PTR:
			printf("\tmov BYTE PTR [RBP], BL\n");
			printf("\tadd RBP, %d\n", ops[i].mem_offset);
			printf("\tmov BL, BYTE PTR [RBP]\n");
			break;
		case OP_ADD:
			printf("\tadd BL, %u\n", ops[i].add);
			break;
		case OP_LABEL:
			printf("l%u:\n", ops[i].label);
			break;
		case OP_JMP:
			printf("\tjmp l%u\n", ops[i].label);
			break;
		case OP_BR_IFNZ:
			printf("\tcmp BL, 0\n");
			printf("\tjnz l%u\n", ops[i].label);
			break;
		case OP_READ:
			printf("\t// TODO: PANIC!\n");
			break;
		case OP_WRITE:
			printf("\tmov RAX, 1\n");
			printf("\tmov RDI, 0\n");
			printf("\tmov RSI, RBP\n");
			printf("\tmov RDX, 1\n");
			printf("\tsyscall\n");
			break;
		}
	}

	printf("\tmov RAX, 60\n");
	printf("\tmov RDI, 42\n");
	printf("\tsyscall\n");
}

int main(int argc, char **argv) {
	ops = malloc(sizeof(struct OpCode) * MAX_CODE_LEN);
	ops_into = malloc(sizeof(struct OpCode) * MAX_CODE_LEN);

	char *source = NULL;

	FILE *sfp = fopen(argv[1], "rb");
	if (!sfp) return 3;

	source = malloc(8 * 1024 * 1024);

	ssize_t amt = fread(source, 1, 8 * 1024 * 1024, sfp);
	if (amt < 0) return 9;
	source[amt] = 0;

	fclose(sfp);

	uint32_t stack[64];
	uint32_t stack_i = 0;

	uint32_t last_label = 0;

	int last = 0;
	for (; *source; source++) {
		switch (*source) {
		case '+':
			ops_into[last++] = (struct OpCode) {
				.kind = OP_ADD,
				.add = 1,
			};
			break;
		case '-':
			ops_into[last++] = (struct OpCode) {
				.kind = OP_ADD,
				.add = 255,
			};
			break;
		case '<':
			ops_into[last++] = (struct OpCode) {
				.kind = OP_ADD_PTR,
				.mem_offset = -1,
			};
			break;
		case '>':
			ops_into[last++] = (struct OpCode) {
				.kind = OP_ADD_PTR,
				.mem_offset = 1,
			};
			break;
		case '[':
			stack[stack_i++] = last;
			ops_into[last++] = (struct OpCode) {
				.kind = OP_JMP,
				.label = last_label,
			};
			ops_into[last++] = (struct OpCode) {
				.kind = OP_LABEL,
				.label = last_label,
			};
			last_label++;
			break;
		case ']': {
			uint32_t jump_tgt = stack[--stack_i];
			ops_into[jump_tgt].label = last_label;
			ops_into[last++] = (struct OpCode) {
				.kind = OP_LABEL,
				.label = last_label,
			};
			ops_into[last++] = (struct OpCode) {
				.kind = OP_BR_IFNZ,
				.label = ops_into[jump_tgt + 1].label,
			};
			last_label++;
		} break;
		case '.':
			ops_into[last++] = (struct OpCode) {
				.kind = OP_WRITE,
			};
			break;
		case ',':
			ops_into[last++] = (struct OpCode) {
				.kind = OP_READ,
			};
			break;
		}
	}

	ops_into[last++] = (struct OpCode) {
		.kind = OP_END,
		.add = 1,
	};

	swap();

	coalesce();
	swap();

	runCodegen();
}
