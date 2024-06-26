#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

#include "threads/synch.h"
#include "threads/malloc.h"
#include "userprog/syscall.h"
#include "filesys/inode.h"

static void process_cleanup (void);
static bool load (char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* Lock for Executables. */
//struct lock exe_lock;

/* NEWCODE : additional functions for file descriptors. */
int process_add_file(struct file* file){
	//insert file to thread's fd table.
	struct thread* current = thread_current();
	struct fd* file_desc = malloc(sizeof(struct fd));
	//if memory allocation failed : return -1.
	if(file_desc == NULL){
		file_close(file);
		return -1;
	}
	file_desc->dir = NULL;
#ifdef EFILESYS
	struct inode* inode = file_get_inode(file);
	if(inode_isdir(inode)){
		struct dir* dir = dir_open(inode_reopen(inode));
		file_desc->dir = dir;
	}
#endif
	file_desc->file = file;
	file_desc->fd_num = current->max_fd;
	list_push_back(&current->fd_table, &file_desc->elem);
	current->max_fd ++;
	
	return file_desc->fd_num;
}

struct file* process_get_file(int fd){
	//get a file given the fd number.
	struct thread* current = thread_current();
	struct list_elem* e;
	struct file* resultfile = NULL;
	
	//search through fd table.
	for (e = list_begin (&current->fd_table); e != list_end (&current->fd_table); e = list_next (e)){
		struct fd* fid = list_entry(e, struct fd, elem);
		if(fid->fd_num == fd){
			resultfile = fid->file;
			break;
		}
	}
	
	return resultfile;
}

struct list_elem* search(struct list* list, int fd){
	struct list_elem* e;
	struct list_elem* result = NULL;
	for (e = list_begin (list); e != list_end (list); e = list_next (e)){
		struct fd* fid = list_entry(e, struct fd, elem);
		if(fid->fd_num == fd){
			result = e;
			break;
		}
	}
	return result;
}

void process_close_file(int fd){
	//USE : void file_close (struct file *file)
	struct thread* current = thread_current();
	struct list_elem* e;
	
	e = search(&current->fd_table, fd);
	if(e == NULL){
		exit(-1);
	}
	struct fd* fid = list_entry(e, struct fd, elem);
	list_remove(e);
	file_close(fid->file);
#ifdef EFILESYS
	if(fid->dir != NULL)
		dir_close(fid->dir);
#endif
	//palloc_free_page(fid);
	free(fid);
}

/* Additional functions for Process Hierarchy. */
struct pcb* get_child_process(int pid){
	//look for a process with tid_t "pid". if none, return NULL.
	struct thread* current = thread_current();
	struct pcb* result = NULL;
	struct list_elem* e;
	
	//search through child list.
	for (e = list_begin (&current->child_list); e != list_end (&current->child_list); e = list_next (e)){
		struct pcb* child = list_entry(e, struct pcb, elem);
		if(child->tid == pid){
			result = child;
			break;
		}
	}
	
	return result;
}
/* ENDOFNEWCODE */


/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* NEWCODE : save the argument intr_frame in the thread struct. */
	tid_t child;
	struct thread* current = thread_current();
	current->f_fork = if_;
	/* Clone current thread to new thread.*/
	child = thread_create (name, PRI_DEFAULT, __do_fork, thread_current ());
	if(child == TID_ERROR) return child;
	
	sema_down(&current->load_sema);
	//if the child succeeded at resource duplication, then its a success. Otherwise, it would have exited (-1).
	struct pcb* child_pcb = get_child_process(child);
	if(child_pcb->exit_status == -1){
		//printf("at parent : %d, child : %d, fork fail due to resource duplication fail.\n", current->tid, child_pcb->tid);
		int wait = process_wait(child);
		//printf("child exited with status %d, and pcb is freed.\n", wait);
		return TID_ERROR;
	}
	
	current->f_fork = NULL;
	return child;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if(is_kernel_vaddr(va)) {
		//printf("kernel page!\n");
		return true;
	}

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
	bool succ = true;
	/* NEWCODE : pass the parent's f_fork, which is passed from process_fork(). */
	parent_if = parent->f_fork;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));
	if_.R.rax = 0;

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif
#ifdef EFILESYS
	if (parent->current_dir != NULL)
		current->current_dir = dir_reopen(parent->current_dir);		//child inherits parent's CURRENT directory.
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	//current(child)'s fd table : current->fd_table, parent's fd table : parent->fd_table.
	struct list_elem* e;
	for(e = list_begin(&parent->fd_table); e != list_end(&parent->fd_table); e = list_next(e)){
		struct fd* parent_fd = list_entry(e, struct fd, elem);
		//USE : struct file* file_duplicate (struct file *file)
		struct file* copy = file_duplicate(parent_fd->file);
		if(copy == NULL){
			//printf("file copy fail!\n");
			goto error;
		}
		//add this copy to the current(child)'s fd table.
		int open = process_add_file(copy);
		if(open == -1){
			//printf("fd allocation failed!!\n");
			goto error;
		}
	}
	
	//let parent return from fork().
	sema_up(&parent->load_sema);
	
	process_init ();
	

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
	
error:
	current->pcb->exit_status = -1;
	sema_up(&parent->load_sema);
	exit(-1);
	//thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;
	
	/* NEWCODE */
	//tokenizing file name.
	char* ret_ptr;
	file_name = strtok_r(file_name, " ", &ret_ptr);
	/* ENDOFNEWCODE */

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();
	
#ifdef VM
	/* Init SPT.*/
	supplemental_page_table_init(&thread_current()->spt);
#endif
#ifdef EFILESYS
	thread_current()->current_dir = dir_open_root();
#endif

	/* And then load the binary */
	success = load (file_name, &_if);

	/* If load failed, quit. */
	palloc_free_page (file_name);
	if (!success)
		return -1;

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	
	/* NEWCODE */
	//1. search for the child with "child_tid".
	struct thread* current = thread_current();
	struct pcb* child;
	int child_exitstatus;
	child = get_child_process(child_tid);
	if(child == NULL) return -1;
	//2. found child. check conditions.
	//already waiting?
	if(child->waiting){
		return -1;
	}
	else{
		child->waiting = true;
	}
	//use semaphore to wait for child.
	if(!child->exited){
		sema_down(&child->thread->exit_sema);
	}
	
	child_exitstatus = child->exit_status;
	//remove from child list & DESTROY child.
	list_remove(&child->elem);
	//palloc_free_page(child);
	free(child);
	/* ENDOFNEWCODE */
	
	return child_exitstatus;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *current = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	 
	/* NEWCODE */
	//Process resouce cleanup - allocated file descriptors.
	struct list_elem* e;
	while(!list_empty(&current->fd_table)){
		e = list_pop_front(&current->fd_table);
		struct fd* fid = list_entry(e, struct fd, elem);
		file_close(fid->file);
#ifdef EFILESYS
		if(fid->dir != NULL)
			dir_close(fid->dir);
#endif
		free(fid);
	}
	while(!list_empty(&current->child_list)){
		e = list_pop_front(&current->child_list);
		struct pcb* pcb = list_entry(e, struct pcb, elem);
		//palloc_free_page(pcb);
		free(pcb);
	}
	current->pcb->exited = true;
	//save exit status to pcb.
	current->pcb->exit_status = current->exit_status;
	//printf("child exit status : %d -> pcb exit status : %d\n", current->exit_status, current->pcb->exit_status);
	sema_up(&current->exit_sema);
	
	//Allow write to executable.
	if(current->executable != NULL) {
		file_allow_write(current->executable);
		file_close(current->executable);
	}
	/* ENDOFNEWCODE */

	process_cleanup ();
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif
#ifdef EFILESYS
	if(curr->current_dir != NULL)
		dir_close(curr->current_dir);
#endif
	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

static void setup_argument(const char *file_name, struct intr_frame *if_){
	/* NEWCODE */
	//Argument parsing using strtok_r(). stack pointer : if_->rsp
	int i;
	int argc = 0;
	char* ret_ptr;
	char* next_ptr;
	char command[64];
	strlcpy(command, file_name, 64);
	char* argv[32];
	
	ret_ptr = strtok_r(command, " ", &next_ptr);
	while(ret_ptr){
		//for each keyword.
		//printf("%s\n", ret_ptr);
		argv[argc] = ret_ptr;
		argc++;
		ret_ptr = strtok_r(NULL, " ", &next_ptr);
	}
	char* argv_addr[argc];
	//1. saving arguments onto stack in reverse order : argv[argc-1] -> argv[argc-2] -> ..
	int length = 0;
	for(i=argc-1; i >=0; i--){
		length = strlen(argv[i]) + 1;
		if_->rsp -= length;
		memcpy((void*)if_->rsp, argv[i], length);
		argv_addr[i] = (char*) if_->rsp;
	}
	//round to multiple of 8.
	if_->rsp = (if_->rsp) & 0xfffffff8;
	if_->rsp -= 8;
	*((char**) if_->rsp) = 0;
	for(i=argc-1; i >=0; i--){
		if_->rsp -= 8;
		*((char**) if_->rsp) = argv_addr[i];
	}
	if_->R.rsi = (uint64_t) if_->rsp;
	if_->R.rdi = (uint64_t) argc;
	//fake return address.
	if_->rsp -= 8;
	*((void**) if_->rsp) = 0;
	//printf("rsp : %X\n", if_->rsp);
	/* ENDOFNEWCODE */
}


/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	char* ret_ptr;

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	lock_acquire(&filesys_lock);
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		lock_release(&filesys_lock);
		goto done;
	}
	lock_release(&filesys_lock);
	/* NEWCODE : Deny Write to Executables */
	t->executable = file;
	file_deny_write(file);
	//process_add_file(file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;
		
	/* Set up argument */
	//printf("current name : %s\ncurrent exec : %d\n", t->name, t->exec);
	if(!t->exec){
		strlcpy(t->name, file_name, 16);
	}
	//strlcpy(t->name, file_name, 16);
	*(file_name + strlen(file_name)) = ' ';
	if(strlen(file_name) > 128){
		file_name = strtok_r(file_name, " ", &ret_ptr);
	}
	setup_argument(file_name, if_);
	//hex_dump(if_->rsp, (void*)if_->rsp, KERN_BASE - if_->rsp, true);

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	//file_close (file);
	return success;
}




/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: VA is available when calling this function. */
	//The info we need from AUX : file struct pointer, page_read_bytes, page_zero_bytes.
	//Where is the physical FRAME? -> page->frame->kva
	uint8_t *kpage = page->frame->kva;
	size_t page_read_bytes = ((struct lazy_aux*)aux)->page_read_bytes;
	size_t page_zero_bytes = ((struct lazy_aux*)aux)->page_zero_bytes;
	off_t ofs = ((struct lazy_aux*)aux)->offset;
	struct file* file = ((struct lazy_aux*)aux)->executable;
	if (kpage == NULL)
		return false;
	// Load this page.
	file_seek(file, ofs);
	if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
		//printf("load failed..\n");
		return false;
	}
	memset (kpage + page_read_bytes, 0, page_zero_bytes);
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		void *aux = NULL;
		struct lazy_aux* AUX = malloc(sizeof(struct lazy_aux));
		AUX->executable = file;
		AUX->page_read_bytes = page_read_bytes;
		AUX->page_zero_bytes = page_zero_bytes;
		AUX->offset = ofs;
		aux = AUX;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux)){
			//printf("Alloc failed..\n");
			return false;
		}
		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += PGSIZE;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */
	success = vm_alloc_page(VM_MARKER_0 + VM_ANON, stack_bottom, true);
	if(success){
		success = vm_claim_page(stack_bottom);
	}
	if(success){
		if_->rsp = USER_STACK;
	}
	return success;
}
#endif /* VM */
