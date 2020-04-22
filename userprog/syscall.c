#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

#include "threads/synch.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* lock for filesys. */
struct lock filesys_lock;

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
	lock_init(&filesys_lock);
	
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}



/* NEWCODE */
//this is a function for terminating a process with exit status "status". termination message will be printed @ process_exit().
void exit(int status){
	//release lock before exit.
	if(lock_held_by_current_thread(&filesys_lock)){
		lock_release(&filesys_lock);
	}
	struct thread* current = thread_current();
	current->exit_status = status;
	thread_exit();
}

//this is a function for wait : Waits for a child process "pid" and retrieves the child's exit status.
int wait (tid_t pid){
	return process_wait(pid);
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

//this is a function for reading in multiple bytes of data from user pointer. check validity of pointer each time.
static void getmultiple_user(void* addr, void* dest, size_t size){
	int32_t v;
	size_t i;
	for(i=0; i<size; i++){
		//check validity of address.
		check_address(addr+i);
		//v = *((int*) (addr+i));
		//*(char*)(dest + i) = v & 0xff;
	}
	memcpy(dest, addr, size);
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
	//getmultiple_user((void*)f->rsp, &syscall_num, 4);
	syscall_num = (int) f->R.rax;
	switch(syscall_num){
		case SYS_HALT:
		{
			power_off();
			break;
		}/* Halt the operating system. */
		case SYS_EXIT:
		{
			//one argument, exit status.
			int exit_status;
			getmultiple_user((void*) (f->rsp + 4), &exit_status, sizeof(int));
			
			//call exit.
			exit(exit_status);
			NOT_REACHED();
			break;
		}/* Terminate this process. */
		case SYS_FORK:
		{
			break;
		}/* Clone current process. */
		case SYS_EXEC:
		{
			break;
		}/* Switch current process. */
		case SYS_WAIT:
		{
			//one argument. pid.
			tid_t pid;
			getmultiple_user((void*) (f->rsp + 4), &pid, sizeof(tid_t));
			
			wait(pid);
			NOT_REACHED();
			break;
		}/* Wait for a child process to die. */
		case SYS_CREATE:
		{
			break;
		}/* Create a file. */
		case SYS_REMOVE:
		{
			break;
		}/* Delete a file. */
		case SYS_OPEN:
		{
			break;
		}/* Open a file. */
		case SYS_FILESIZE:
		{
			break;
		}/* Obtain a file's size. */
		case SYS_READ:
		{
			break;
		}/* Read from a file. */
		case SYS_WRITE:
		{
			break;
		}/* Write to a file. */
		case SYS_SEEK:
		{
			break;
		}/* Change position in a file. */
		case SYS_TELL:
		{
			break;
		}/* Report current position in a file. */
		case SYS_CLOSE:
		{
			break;
		}/* Close a file. */
		
		default:
		{
			printf("Invalid system call number : %d\n", syscall_num);
			exit(-1);
			power_off();
			NOT_REACHED();
			break;
		}
	}
	/* ENDOFNEWCODE */
	
	printf ("system call!\n");
	thread_exit ();
}
