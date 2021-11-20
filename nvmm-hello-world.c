#include <sys/mman.h>
#include <sys/stat.h>

#include <machine/psl.h>
#include <machine/segments.h>

#include <x86/specialreg.h>

#include <err.h>
#include <fcntl.h>
#include <nvmm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

size_t guest_memory_space = 4096;
uintptr_t entry_address = 0;
uintptr_t data_address = 2048;

static void
nvmm_io_callback(struct nvmm_io *io)
{
	if (io->port == 0x3f8 && io->in != 1) {
		uint32_t size = io->size;
		printf("%.*s", size, (char *)io->data);
	}
}

int main(int argc, char *argv[])
{
	struct nvmm_machine	     mach;
	struct nvmm_vcpu	     vcpu;
	void			    *hva;
	gpaddr_t		     gpa = 0xFFFF0000;
	struct nvmm_assist_callbacks cbs = {
		.io = nvmm_io_callback,
	};

	uint8_t inst[] = {
		0x8a, 0x01, /* mov al,[ebx] */
		0x43,	    /* inc ebx */
		0x3c, 0x00, /* cmp al,0x0 */
		0x74, 0x03, /* jz (to hlt) */
		0xee,	    /* outpu */
		0xeb, 0xf6, /* jump short (to 0x00) */
		0xf4,	    /* hlt */
	};

	if (argc != 2) {
		fprintf(stderr, "require an argument to output string\n");
		exit(1);
	}

	printf("---- prepare vm ----\n");

	if (nvmm_init() == -1)
		err(EXIT_FAILURE, "unable to init NVMM");

	if (nvmm_machine_create(&mach) == -1)
		err(EXIT_FAILURE, "unable to create the VM");

	nvmm_vcpu_create(&mach, 0, &vcpu);
	if (nvmm_vcpu_configure(&mach, &vcpu, NVMM_VCPU_CONF_CALLBACKS, &cbs) ==
	    -1)
		err(EXIT_FAILURE, "unable to configure callbackn");

	hva = mmap(NULL, guest_memory_space, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	nvmm_hva_map(&mach, (uintptr_t)hva, guest_memory_space);
	nvmm_gpa_map(&mach, (uintptr_t)hva, gpa, guest_memory_space,
	    PROT_READ | PROT_WRITE | PROT_EXEC);

	printf("set program to gpa=%p\n", entry_address);
	memcpy(hva, inst, sizeof(inst));

	printf("set string %s to gpa=%p\n", argv[1], data_address);
	strncpy(hva + data_address, argv[1], guest_memory_space);

	nvmm_vcpu_getstate(
	    &mach, &vcpu, NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS);
	vcpu.state->segs[NVMM_X64_SEG_DS].base = 0xFFFF0000;

	vcpu.state->gprs[NVMM_X64_GPR_RIP] = entry_address;
	vcpu.state->gprs[NVMM_X64_GPR_RBX] = data_address;
	vcpu.state->gprs[NVMM_X64_GPR_RDX] = 0x3f8;
	nvmm_vcpu_setstate(
	    &mach, &vcpu, NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS);

	printf("---- start vm ----\n");
	while (1) {
		nvmm_vcpu_run(&mach, &vcpu);

		/* Process the exit reasons. */
		switch (vcpu.exit->reason) {
		case NVMM_VCPU_EXIT_NONE:
			break;
		case NVMM_VCPU_EXIT_HALTED:
			printf("\n---- VM finished ----\n");
			return 0;
			/* THE PROCESS EXITS, THE VM GETS DESTROYED. */
		case NVMM_VCPU_EXIT_IO:
			if (nvmm_assist_io(&mach, &vcpu) < 0) {
				nvmm_vcpu_dump(&mach, &vcpu);
				errx(EXIT_FAILURE, "nvmm_assist_io");
			}
			break;
		default:
			nvmm_vcpu_dump(&mach, &vcpu);
			errx(EXIT_FAILURE, "unknown exit reason %x",
			    vcpu.exit->reason);
		}
	}

	return 0;
}
