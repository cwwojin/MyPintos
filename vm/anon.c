/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */

#include "vm/vm.h"
#include "devices/disk.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* DO NOT MODIFY BELOW LINE */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

static struct list swap_list;	//swap table.
static struct lock swap_lock;	//swap lock -> use this when modifying swap slots.
static int nSlots;		//MAX #. of slots, index goes up to 0 ~ (nSlots - 1).
static void swap_init(void);

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* TODO: Set up the swap_disk. */
	swap_disk = disk_get (1,1);	//1:1 - swap
	list_init(&swap_list);
	lock_init(&swap_lock);
	swap_init();
}

static void swap_init(void){
	nSlots = ((int) disk_size(swap_disk)) / SECTORS_PER_PAGE;
	printf("nSlots : %d\n",nSlots);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	anon_page->init = page->uninit.init;
	anon_page->aux = page->uninit.aux;
	anon_page->type = page->uninit.type;
	anon_page->slot = NULL;
}

/* Get an available swap slot. if None exist and total slots < nSlots, then allocate a NEW one. */
static struct swap_slot* get_available_slot(void){
	struct list_elem* e;
	struct swap_slot* target = NULL;
	lock_acquire(&swap_lock);
	for(e = list_begin(&swap_list); e != list_end(&swap_list); e = list_next(e)){
		struct swap_slot* slot = list_entry(e, struct swap_slot, elem);
		if(slot->free){	//This slot is FREE. choose this.
			slot->free = false;
			target = slot;
			break;
		}
	}
	if(target == NULL){	//None of the slots were available, allocate a NEW one.
		int size = (int) list_size(&swap_list);
		if(size > nSlots){
			printf("No more swap-slots!!!\n");
			lock_release(&swap_lock);
			return NULL;
		}
		target = malloc(sizeof(struct swap_slot));
		target->free = false;
		target->slotNo = size;
		list_push_back(&swap_list, &target->elem);
		printf("Added new swap-slot No.%d\n", target->slotNo);
	}
	lock_release(&swap_lock);
	ASSERT(target != NULL);
	return target;
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;
	int i;
	struct swap_slot* slot = anon_page->slot;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	int i;
	struct swap_slot* slot = get_available_slot();			//get a swap slot;
	disk_sector_t sec_no = slot->slotNo * SECTORS_PER_PAGE;		//set sector Number : slotNo * 8
	printf("Swapping out ANON-PAGE 0x%X to swap slot %d <-> sector %d\n", page->va, slot->slotNo, sec_no);
	//Use VA or KVA??
	//void* buffer = page->va;
	void* buffer = page->frame->kva;
	for(i = 0; i < SECTORS_PER_PAGE; i++){	//USE : void disk_write (struct disk *d, disk_sector_t sec_no, const void *buffer)
		disk_write(swap_disk, sec_no, buffer);
		buffer += DISK_SECTOR_SIZE;
		sec_no++;
	}
	anon_page->slot = slot;		//save slot to anon_page.
	return true;
}

/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	if(anon_page->aux != NULL){
		free(anon_page->aux);
	}
}
