/* vm.c: Generic interface for virtual memory objects. */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "vm/uninit.h"
#include <debug.h>

static struct lock frame_lock;	//frame table lock.

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* TODO: Your code goes here. */
	list_init(&frame_list);
	lock_init(&frame_lock);
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		//printf("allocating new page of type : %d at addr : %X, writable : %d\n",VM_TYPE(type), upage, writable);
		struct page* page = malloc(sizeof(struct page));
		switch(VM_TYPE(type)){		//uninit_new (page,va,init,type,aux, bool(*initializer))
			case VM_ANON:
				uninit_new(page, upage, init, type, aux, anon_initializer);
				break;
			case VM_FILE:
				uninit_new(page, upage, init, type, aux, file_map_initializer);
				break;
			default:
				break;
		}
		page->writable = writable;
		/* TODO: Insert the page into the spt. */
		return spt_insert_page(&thread_current()->spt, page);
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	struct page p;
	struct hash_elem *e;
	p.va = va;
	e = hash_find(&spt->hash, &p.hash_elem);
	page = (e != NULL ? hash_entry(e, struct page, hash_elem) : NULL);

	return page;
}

/* Insert PAGE into spt with validation. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	bool succ = false;
	/* TODO: Fill this function. */
	if(hash_insert(&spt->hash, &page->hash_elem) == NULL){
		succ = true;
	}

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	if(hash_delete (&spt->hash, &page->hash_elem) != NULL){
		vm_dealloc_page (page);
	}
}

/* Get the struct frame, that will be evicted. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
	//policy : FIFO.
	lock_acquire(&frame_lock);
	victim = list_entry(list_pop_front(&frame_list), struct frame, elem);
	lock_release(&frame_lock);
	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
static struct frame *
vm_evict_frame (void) {
	struct frame* victim = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	ASSERT(victim != NULL);
	struct page* victim_page = victim->page;
	struct thread* victim_owner = victim->owner;
	if(!swap_out(victim_page)){
		printf("swap-out failed at victim page 0x%X\n",victim_page->va);
		return NULL;
	}
	//USE : void pml4_clear_page (uint64_t *pml4, void *upage)
	pml4_clear_page(victim_owner->pml4, victim_page->va);
	return victim;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	void* new = palloc_get_page(PAL_USER);	//get a page from the user pool. NULL if allocation fails.
	if(new != NULL){
		//initialize frame.
		frame = malloc(sizeof(struct frame));
		if(frame != NULL){
			frame->kva = new;
			frame->page = NULL;
			frame->owner = thread_current();
			frame->cnt = 1;
			list_push_back(&frame_list, &frame->elem);
		}
	}
	else{	//Evict a frame and retrieve it. Use the page @ frame->kva.
		frame = vm_evict_frame();
		if(frame != NULL){
			frame->page = NULL;
			frame->owner = thread_current();
			frame->cnt = 1;
			list_push_back(&frame_list, &frame->elem);
		}
	}
	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
static bool
vm_stack_growth (void *addr UNUSED) {
	bool success;
	success = vm_alloc_page(VM_MARKER_0 + VM_ANON, pg_round_down(addr), true);
	if(success){
		success = vm_claim_page(pg_round_down(addr));
	}
	return success;
}

/* Handle the fault on write_protected page */
static bool
vm_handle_wp (struct page *page UNUSED) {
	//return page->writable;
	if(!page->writable){	//Check if write-protected page.
		return false;
	}
	else{				//Writable is TRUE, so this is a COPY-ON-WRITE!!
		//printf("COW fault @ thread %d, PAGE 0x%X, writable : %d, \n", thread_current()->tid, page->va, page->writable);
		void* old_kva = pml4_get_page(thread_current()->pml4, page->va);
		pml4_clear_page(thread_current()->pml4, page->va);	//Remove mapping.
		if(!vm_do_claim_page(page)){
			return false;
		}
		ASSERT(page->frame != NULL);
		memcpy(page->frame->kva, old_kva, PGSIZE);		//Copy contents. What about file-mapped pages??
		return true;
	}
	
}

/* Return true on success */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	bool accessing_stack;
	bool success;
	/* TODO: Validate the fault */
	page = spt_find_page(spt, pg_round_down(addr));
	if(page == NULL){	//the page is INVALID.
		accessing_stack = ((addr < USER_STACK) && (USER_STACK - (int) pg_round_down(addr)) <= (PGSIZE << 8) && (uintptr_t)addr >= (f->rsp - 64));
		if(accessing_stack){	//Stack Growth.
			return vm_stack_growth(addr);
		}
		else{		//Not a stack-access, so its a real fault.
			//printf("fault @ 0x%X -> PAGE %X, user : %d, write : %d, not_present : %d\n",addr,pg_round_down(addr),user,write,not_present);
			return false;
		}
	}
	/* TODO: Your code goes here */
	if(write && !not_present){
		return vm_handle_wp(page);
	}

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Free the frame struct & remove it from frame table. */
void
vm_dealloc_frame (struct frame* frame){
	list_remove(&frame->elem);
	free(frame);
}

/* Claim the page that allocate on VA. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	page = spt_find_page(&thread_current()->spt, va);
	ASSERT(page != NULL);

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();
	/* Set links */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	pml4_set_page(thread_current()->pml4, page->va, frame->kva, page->writable);
	//printf("thread %d claimed page 0x%X\n",thread_current()->tid, page->va);

	return swap_in (page, frame->kva);
}

/* NEWCODE : Functions for supplemental page table's hash table. */
/* Returns a hash value for page p. */
unsigned page_hash (const struct hash_elem *p_, void *aux UNUSED) {
	const struct page *p = hash_entry (p_, struct page, hash_elem);
	return hash_bytes (&p->va, sizeof p->va);
}
/* Returns true if page a precedes page b. */
bool page_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED) {
	const struct page *a = hash_entry (a_, struct page, hash_elem);
	const struct page *b = hash_entry (b_, struct page, hash_elem);
	return a->va < b->va;
}


/* Initialize new supplemental page table */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	spt->owner = thread_current();
	hash_init(&spt->hash, page_hash, page_less, NULL);
}

/* Copy supplemental page table from src to dst */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
	struct hash_iterator i;
	hash_first (&i, &src->hash);
	while(hash_next(&i)){
		struct page *p = hash_entry(hash_cur (&i), struct page, hash_elem);	//get the SRC's page.
		//printf("Going to copy page : 0x%X..\n", p->va);
		void* aux = NULL;
		switch(p->uninit.type){
			case VM_ANON :
				if(p->uninit.aux != NULL){
					aux = malloc(sizeof(struct lazy_aux));
					memcpy(aux, p->uninit.aux, sizeof(struct lazy_aux));
				}
				break;
			case VM_FILE :
				if(p->uninit.aux != NULL){
					aux = malloc(sizeof(struct lazy_aux));
					memcpy(aux, p->uninit.aux, sizeof(struct lazy_aux));
				}
				struct file* newfile = file_reopen(((struct lazy_aux*) aux)->executable);
				((struct lazy_aux*) aux)->executable = newfile;
				break;
			default :
				break;
		}
		if(!vm_alloc_page_with_initializer(p->uninit.type, p->va, p->writable, p->uninit.init, aux)){	//page_get_type(p)
			printf("SPT_COPY : failed to allocate page.\n");
			return false;
		}
		struct page* newp = spt_find_page(dst, p->va);
		if(p->frame != NULL){
			/* COPY-ON-WRITE : Instead of claiming page here, just add the pml4 mapping & set write-protected!! */
			pml4_set_page(thread_current()->pml4, newp->va, p->frame->kva, false);
			pml4_clear_page(p->frame->owner->pml4, p->va);
			pml4_set_page(p->frame->owner->pml4, p->va,p->frame->kva,false);
			p->frame->cnt++;
		}
		else if(pml4_get_page(src->owner->pml4, p->va) != NULL){
			pml4_set_page(thread_current()->pml4, newp->va, pml4_get_page(src->owner->pml4, p->va), false);
		}
	}
	return true;
}

void spt_free_page(struct hash_elem* e, void* aux UNUSED){
	struct page* page = hash_entry(e, struct page, hash_elem);
	destroy(page);
	free(page);
}

/* Free the resource hold by the supplemental page table */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	hash_destroy(&spt->hash, spt_free_page);
}
