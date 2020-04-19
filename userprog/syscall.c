#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* Reads a byte at user virtual address UADDR.
 * UADDR must be below KERN_BASE.
 * Returns the byte value if successful, -1 if a segfault
 * occurred. */
static int
get_user (const uint8_t *uaddr) {
    int result;
    asm ("movl $1f, %0; movzbl %1, %0; 1:"
         : "=&a" (result) : "m" (*uaddr));
    return result;
}

/* Writes BYTE to user address UDST.
 * UDST must be below KERN_BASE.
 * Returns true if successful, false if a segfault occurred. */
static bool
put_user (uint8_t *udst, uint8_t byte) {
    int error_code;
    asm ("movl $1f, %0; movb %b2, %1; 1:"
    : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != -1;
}

/* NEWCODE */
//this is a function for terminating a process with exit status "status". termination message will be printed @ process_exit().
void exit(int status){
	struct thread* current = thread_current();
	current->exit_status = status;
	thread_exit();
}



//this is a function for checking if pointer is "valid". If not, call a page fault.
void check_address(void* addr){
	//case 1. NULL pointer.
	if(addr == NULL){
		exit(-1);
	}
	//case 2. addr is in kernal address space
	if(is_kernel_vaddr(addr)){
		exit(-1);
	}
	//case 3. UNMAPPED pointer. check if "addr"'s corresponding page exists in current thread's pml4.
	if(pml4_get_page (thread_current()->pml4, addr) == NULL){
		exit(-1);
	}
}
/* ENDOFNEWCODE*/


/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	struct thread* current = thread_current();
	/* NEWCODE*/
	int syscall_num;
	//get address stored in stack pointer 'rsp'
	check_address((void*)f->rsp);
	syscall_num =  * (int*)vtop(pml4_get_page (current->pml4, f->rsp));
	
	
	
	
	/* ENDOFNEWCODE */
	
	printf ("system call!\n");
	thread_exit ();
}
