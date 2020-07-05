#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

//#include "include/lib/user/syscall.h"
#include <string.h>
#include "threads/synch.h"
#include "threads/init.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/process.h"
#include "devices/input.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

#ifdef VM
#include "vm/vm.h"
#endif
#ifdef EFILESYS
#include "filesys/inode.h"
#include "devices/disk.h"
#endif

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

	//print termination message here.
	printf ("%s: exit(%d)\n", current->name, current->exit_status);
	thread_exit();
}

//this is a function for checking if pointer is "valid". If not, call a page fault.
void check_address(void* addr){
	//case 1. NULL pointer.
	if(addr == NULL){
		exit(-1);
	}
	//case 2. addr is in kernel address space
	if(!is_user_vaddr(addr)){
		exit(-1);
	}
#ifndef VM
	//case 3. UNMAPPED pointer. check if "addr"'s corresponding page exists in current thread's pml4.
	if(pml4_get_page (thread_current()->pml4, addr) == NULL){
		exit(-1);
	}
#else
	//VM : case 3. UNMAPPED pointer. check if "addr"'s corresponding page exists in current thread's Supplemental Page Table.
	struct page* page = spt_find_page(&thread_current()->spt, pg_round_down(addr));
	if(page == NULL){	//Stack Growth from a SYSCALL.
		bool accessing_stack = ((addr < USER_STACK) && (USER_STACK - (int) pg_round_down(addr)) <= (PGSIZE << 8) && (uintptr_t)addr >= (thread_current()->syscall_rsp - 64));
		if(accessing_stack){
			bool success;
			success = vm_alloc_page(VM_MARKER_0 + VM_ANON, pg_round_down(addr), true);
			if(success){
				success = vm_claim_page(pg_round_down(addr));
			}
			if(!success){
				exit(-1);
			}
		}
		else{	//True invalid pointer.
			exit(-1);
		}
	}
#endif
}

#ifdef VM
static void write_permission(void* addr){
	struct page* page = spt_find_page(&thread_current()->spt, pg_round_down(addr));
	//printf("Page : 0x%X, writable : %d\n", pg_round_down(addr), page->writable);
	if(!page->writable){
		exit(-1);
	}
}
#endif VM

//FILESYS - create : create a file with given name & size.
bool create(const char *file, unsigned initial_size){
	//USE : bool filesys_create (const char *name, off_t initial_size)
	bool result;
	check_address((void*) file);

	//use a lock when accessing filesystem.
	lock_acquire(&filesys_lock);
	result = filesys_create(file, initial_size);
	lock_release(&filesys_lock);

	return result;
}

//FILESYS - remove : remove a given file.
bool remove(const char* file){
	//USE : bool filesys_remove (const char *name)
	bool result;
	check_address((void*) file);

	lock_acquire(&filesys_lock);
	result = filesys_remove(file);
	lock_release(&filesys_lock);

	return result;
}

//FILESYS - open : open a file. return the file descriptor. (-1) if file doesn't exist.
int open(const char* file){
	//USE : struct file* filesys_open (const char *name)
	int result;
	struct file* target;
	check_address((void*) file);

	lock_acquire(&filesys_lock);
	target = filesys_open(file);
	if(target == NULL){
		lock_release(&filesys_lock);
		return -1;
	}

	result = process_add_file(target);
	lock_release(&filesys_lock);

	return result;
}

//FILESYS - filesize : return the size of the file, given the fd.
int filesize(int fd){
	//USE : off_t file_length (struct file *file)
	//1. find the file using the fd.
	int result;
	struct file* target;

	lock_acquire(&filesys_lock);
	target = process_get_file(fd);
	if(target == NULL){
		lock_release(&filesys_lock);
		return -1;
	}

	result = file_length(target);
	lock_release(&filesys_lock);

	return result;
}

//FILESYS - close : Closes file descriptor "fd".
void close (int fd){
	lock_acquire(&filesys_lock);
	process_close_file(fd);
	lock_release(&filesys_lock);
}

//FILESYS - read : read SIZE bytes from file FD into BUFFER. Return number of bytes read, -1 if fail.
int read (int fd, void *buffer, unsigned size){
	//USE : off_t file_read (struct file *file, void *buffer, off_t size)
	//validate memory from buffer ~ buffer + size - 1.
	struct thread* current = thread_current();
	unsigned i;
	for(i=0; i< size; i++){
		//check validity of address.
		check_address((void*) (buffer + i));
#ifdef VM
		write_permission((void*) (buffer + i));
#endif
	}
	int result = -1;
	struct file* target;

	lock_acquire(&filesys_lock);
	if(fd == 0){
		//fd = 0, so get input from keyboard using input_getc()
		for(i=0; i< size; i++){
			*(uint8_t*) (buffer + i) = input_getc();
		}
		result = (int) size;
	}
	else{
		//fd != 0, so read from file found with FD.
		target = process_get_file(fd);
		if(target == NULL){
			lock_release(&filesys_lock);
			return -1;
		}
		result = file_read(target, buffer, size);
	}
	lock_release(&filesys_lock);
	return result;
}

//FILESYS - write : Writes size bytes from buffer to the open file fd. Returns the number of bytes actually written.
int write (int fd, const void *buffer, unsigned size){
	//USE : off_t file_write (struct file *file, const void *buffer, off_t size)
	//validate memory from buffer ~ buffer + size - 1.
	struct thread* current = thread_current();
	unsigned i;
	for(i=0; i< size; i++){
		//check validity of address.
		check_address((void*) (buffer + i));
	}
	int result = -1;
	struct file* target;

	lock_acquire(&filesys_lock);
	if(fd == 1){
		//fd = 1, so write to console at once, using putbuf().
		putbuf(buffer, size);
		result = size;
	}
	else{
		//fd != 0, so read from file found with FD.
		target = process_get_file(fd);
		if(target == NULL){
			lock_release(&filesys_lock);
			return -1;
		}
		result = file_write(target, buffer, size);
	}
	lock_release(&filesys_lock);
	return result;
}

//FILESYS - seek : Changes the next byte to be read or written in open file "fd" to "position".
void seek (int fd, unsigned position){
	//USE : void file_seek (struct file *file, off_t new_pos)
	struct file* target;
	lock_acquire(&filesys_lock);
	target = process_get_file(fd);
	if(target == NULL){
		lock_release(&filesys_lock);
		return;
	}
	file_seek(target, position);
	lock_release(&filesys_lock);
}

//FILESYS - tell : Returns the position of the next byte to be read or written in open file "fd".
unsigned tell (int fd){
	//USE : off_t file_tell (struct file *file)
	struct file* target;
	unsigned result = -1;
	lock_acquire(&filesys_lock);
	target = process_get_file(fd);
	if(target == NULL){
		lock_release(&filesys_lock);
		return -1;
	}
	result = file_tell (target);
	lock_release(&filesys_lock);

	return result;
}

//exec : change current process to the executable @ cmd_line.
int exec(const char* cmd_line){
	check_address((void*) cmd_line);
	/*TODO : currently, exec never works because of pml4's cleanup.
	To make it work, the parameter to process_exec "void* f_name" must be a KERNEL VIRTUAL ADDRESS,
	which points to a memory with the exact copy of the user page at "cmd_line".*/
	char* command = palloc_get_page(PAL_ZERO);
	strlcpy(command, cmd_line, strlen(cmd_line)+1);
	thread_current()->exec = true;
	return process_exec((void*)command);
}

//wait : Waits for a child process pid and retrieves the child's exit status.
int wait (tid_t pid){
	return process_wait(pid);
}

//fork : Create new process which is the clone of current process. Clone register values, memory space, file descriptors.
tid_t fork (const char *thread_name, struct intr_frame* if_){
	check_address((void*) thread_name);
	tid_t result;

	result = process_fork(thread_name, if_);
	return result;
}

#ifdef VM
//mmap : Maps "length" bytes of the file "fd" starting from "offset", into VA space at "addr".
void* mmap (void *addr, size_t length, int writable, int fd, off_t offset){
	//2. Get the file "FD".
	struct file* FILE;
	lock_acquire(&filesys_lock);
	FILE = process_get_file(fd);
	if(FILE == NULL){
		lock_release(&filesys_lock);
		return NULL;
	}
	lock_release(&filesys_lock);
	return do_mmap (addr, length, writable, FILE, offset);
}

//munmap : Unmaps the mapping for the address range "addr".
void munmap (void *addr){
	return do_munmap(addr);
}
#endif

#ifdef EFILESYS
//isdir : return TRUE if fd is a file descriptor for a directory.
bool isdir (int fd){
	struct file* FILE;
	lock_acquire(&filesys_lock);
	FILE = process_get_file(fd);
	if(FILE == NULL){
		lock_release(&filesys_lock);
		return NULL;
	}
	lock_release(&filesys_lock);
	return do_isdir(FILE);
}
//inumber : return fd's inode inumber.
int inumber (int fd){
	struct file* FILE;
	lock_acquire(&filesys_lock);
	FILE = process_get_file(fd);
	if(FILE == NULL){
		lock_release(&filesys_lock);
		return NULL;
	}
	lock_release(&filesys_lock);
	struct inode* inode = file_get_inode(FILE);
	disk_sector_t inumber = inode_get_inumber(inode);
	return (int) inumber;
}
#endif
/* ENDOFNEWCODE*/


/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	struct thread* current = thread_current();
	/* NEWCODE*/
	int syscall_num;
	//check validity of stack pointer.
	check_address((void*)f->rsp);
#ifdef VM
	current->syscall_rsp = f->rsp;
#endif
	//get the system call number from "rax".
	syscall_num = (int) f->R.rax;
	//printf("systemcall number : %d\n", syscall_num);
	switch(syscall_num){
		case SYS_HALT:
		{
			power_off();
			NOT_REACHED();
			break;
		}/* Halt the operating system. */
		case SYS_EXIT:
		{
			//one argument, exit status.
			int exit_status;
			exit_status = (int) f->R.rdi;
			//call exit.
			exit(exit_status);
			NOT_REACHED();
			break;
		}/* Terminate this process. */
		case SYS_FORK:
		{
			//one argument, thread_name.
			char* thread_name;
			tid_t result;
			thread_name = (char*) f->R.rdi;

			result = fork(thread_name, f);
			//printf("fork result (child pid) : %d\n", result);
			f->R.rax = (uint64_t) result;
			break;
		}/* Clone current process. */
		case SYS_EXEC:
		{
			//one argument, cmd_line.
			char* cmd_line;
			cmd_line = (char*) f->R.rdi;
			int result;

			//printf("start exec : %s\n", cmd_line);
			result = exec(cmd_line);
			f->R.rax = (uint64_t) result;
			//NOT_REACHED();
			break;
		}/* Switch current process. */
		case SYS_WAIT:
		{
			//one argument. pid.
			tid_t pid;
			int result;
			pid = (int) f->R.rdi;

			//printf("parent %d will wait for child : %d\n", thread_current()->tid, pid);
			result = wait(pid);
			//printf("wait result : %d\n", result);
			f->R.rax = (uint64_t) result;
			break;
		}/* Wait for a child process to die. */
		case SYS_CREATE:
		{
			//2 arguments. file, initial_size.
			char* file;
			unsigned initial_size;
			bool result;
			file = (char*) f->R.rdi;
			initial_size = (unsigned) f->R.rsi;

			result = create(file, initial_size);
			//return value -> rax.
			f->R.rax = (uint64_t) result;
			break;
		}/* Create a file. */
		case SYS_REMOVE:
		{
			//one argument. file.
			char* file;
			bool result;
			file = (char*) f->R.rdi;

			result = remove(file);
			f->R.rax = (uint64_t) result;
			break;
		}/* Delete a file. */
		case SYS_OPEN:
		{
			//one argument. file.
			char* file;
			int result;
			file = (char*) f->R.rdi;

			result = open(file);
			f->R.rax = (uint64_t) result;
			break;
		}/* Open a file. */
		case SYS_FILESIZE:
		{
			//one argument. filedescriptor.
			int fd;
			int result;
			fd = (int) f->R.rdi;

			result = filesize(fd);
			f->R.rax = (uint64_t) result;
			break;
		}/* Obtain a file's size. */
		case SYS_READ:
		{
			//3 arguments. fd, buffer, size.
			int fd;
			void *buffer;
			unsigned size;
			int result;
			fd = (int) f->R.rdi;
			buffer = (void*) f->R.rsi;
			size = (unsigned) f->R.rdx;
			//printf("READ -> fd: rdi = %d, buffer: rsi = %d, size: rdx = %d\n", fd, buffer, size);

			result = read(fd, buffer, size);
			f->R.rax = (uint64_t) result;
			break;
		}/* Read from a file. */
		case SYS_WRITE:
		{
			//3 arguments. fd, buffer, size
			int fd;
			void *buffer;
			unsigned size;
			int result;
			fd = (int) f->R.rdi;
			buffer = (void*) f->R.rsi;
			size = (unsigned) f->R.rdx;
			//printf("WRITE -> fd: rdi = %d, buffer: rsi = %X, size: rdx = %d\n", fd, buffer, size);

			result = write(fd, buffer, size);
			f->R.rax = (uint64_t) result;
			break;
		}/* Write to a file. */
		case SYS_SEEK:
		{
			//2 arguments. fd, position.
			int fd;
			unsigned position;

			fd = (int) f->R.rdi;
			position = (unsigned) f->R.rsi;
			seek(fd, position);
			break;
		}/* Change position in a file. */
		case SYS_TELL:
		{
			//one argument. fd.
			int fd;
			unsigned result;

			fd = (int) f->R.rdi;
			result = tell(fd);
			f->R.rax = (uint64_t) result;
			break;
		}/* Report current position in a file. */
		case SYS_CLOSE:
		{
			//one argument. fd.
			int fd;
			fd = (int) f->R.rdi;
			close(fd);
			break;
		}/* Close a file. */
#ifdef VM
		case SYS_MMAP:
		{
			//5 arguments. addr, length, writable, fd, offset.
			void* addr = (void*) f->R.rdi;
			size_t length = (size_t) f->R.rsi;
			int writable = (int) f->R.rdx;
			int fd = (int) f->R.r10;
			off_t offset = (off_t) f->R.r8;
			void* result;
			//printf("addr : 0x%X, length : %d, writable : %d, fd : %d, offset : %d\n", addr, length, writable, fd, offset);

			result = mmap(addr, length, writable, fd, offset);
			f->R.rax = (uint64_t) result;
			break;
		}/* Map a file to file-mapped pages. */
		case SYS_MUNMAP:
		{
			//one argument. addr.
			void* addr = (void*) f->R.rdi;
			munmap(addr);
			break;
		}/* Remove mapping of file-mapped pages. */
#endif
#ifdef EFILESYS
		case SYS_CHDIR:
		{
			break;
		}/* Change the current directory. */
		case SYS_MKDIR:
		{
			break;
		}/* Create a directory. */
		case SYS_READDIR:
		{
			break;
		}/* Reads a directory entry. */
		case SYS_ISDIR:
		{
			//one argument. fd.
			int fd;
			bool result;
			fd = (int) f->R.rdi;
			result = isdir(fd);
			f->R.rax = (uint64_t) result;
			break;
		}/* Tests if a fd represents a directory. */
		case SYS_INUMBER:
		{
			//one argument. fd.
			int fd;
			int result;
			fd = (int) f->R.rdi;
			result = inumber(fd);
			f->R.rax = (uint64_t) result;
			break;
		}/* Returns the inode number for a fd. */
		case SYS_SYMLINK:
		{
			break;
		}/* Create a symbolic link. */
#endif
		default:
		{
			printf("Invalid system call number : %d\n", syscall_num);
			exit(-1);
			break;
		}
	}
	/* ENDOFNEWCODE */
	//printf ("system call!\n");
}
