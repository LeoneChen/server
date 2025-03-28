/* Copyright (c) 2001, 2010, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/*
  Function to handle quick removal of duplicates
  This code is used when doing multi-table deletes to find the rows in
  reference tables that needs to be deleted.

  The basic idea is as follows:

  Store first all strings in a binary tree, ignoring duplicates.
  When the tree uses more memory than 'max_heap_table_size',
  write the tree (in sorted order) out to disk and start with a new tree.
  When all data has been generated, merge the trees (removing any found
  duplicates).

  The unique entries will be returned in sort order, to ensure that we do the
  deletes in disk order.
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_sort.h"
#include "queues.h"                             // QUEUE
#include "my_tree.h"                            // element_count
#include "uniques.h"	                        // Unique
#include "sql_sort.h"

int unique_write_to_file(void* key_, element_count, void *unique_)
{
  uchar *key= static_cast<uchar *>(key_);
  Unique *unique= static_cast<Unique *>(unique_);
  /*
    Use unique->size (size of element stored in the tree) and not
    unique->tree.size_of_element. The latter is different from unique->size
    when tree implementation chooses to store pointer to key in TREE_ELEMENT
    (instead of storing the element itself there)
  */
  return my_b_write(&unique->file, key, unique->size) ? 1 : 0;
}

int unique_write_to_file_with_count(void* key_, element_count count, void *unique_)
{
  uchar *key= static_cast<uchar *>(key_);
  Unique *unique= static_cast<Unique *>(unique_);
  return my_b_write(&unique->file, key, unique->size) ||
                 my_b_write(&unique->file, reinterpret_cast<uchar *>(&count),
                            sizeof(element_count))
             ? 1
             : 0;
}

int unique_write_to_ptrs(void* key_, element_count, void *unique_)
{
  uchar *key= static_cast<uchar *>(key_);
  Unique *unique= static_cast<Unique *>(unique_);
  memcpy(unique->sort.record_pointers, key, unique->size);
  unique->sort.record_pointers+=unique->size;
  return 0;
}

int unique_intersect_write_to_ptrs(void* key_, element_count count, void *unique_)
{
  uchar *key= static_cast<uchar *>(key_);
  Unique *unique= static_cast<Unique *>(unique_);
  if (count >= unique->min_dupl_count)
  {
    memcpy(unique->sort.record_pointers, key, unique->size);
    unique->sort.record_pointers+=unique->size;
  }
  else
    unique->filtered_out_elems++;
  return 0;
}


Unique::Unique(qsort_cmp2 comp_func, void * comp_func_fixed_arg,
	       uint size_arg, size_t max_in_memory_size_arg,
               uint min_dupl_count_arg)
  :max_in_memory_size(max_in_memory_size_arg),
   size(size_arg),
   elements(0)
{
  my_b_clear(&file);
  min_dupl_count= min_dupl_count_arg;
  full_size= size;
  if (min_dupl_count_arg)
    full_size+= sizeof(element_count);
  with_counters= MY_TEST(min_dupl_count_arg);
  init_tree(&tree, (max_in_memory_size / 16), 0, size, comp_func,
            NULL, comp_func_fixed_arg, MYF(MY_THREAD_SPECIFIC));
  /* If the following fail's the next add will also fail */
  my_init_dynamic_array(PSI_INSTRUMENT_ME, &file_ptrs, sizeof(Merge_chunk), 16,
                        16, MYF(MY_THREAD_SPECIFIC));
  /*
    If you change the following, change it in get_max_elements function, too.
  */
  max_elements= (ulong) (max_in_memory_size /
                         ALIGN_SIZE(sizeof(TREE_ELEMENT)+size));
  if (!max_elements)
    max_elements= 1;

  (void) open_cached_file(&file, mysql_tmpdir,TEMP_PREFIX, DISK_BUFFER_SIZE,
                          MYF(MY_WME));
}


/*
  Calculate log2(n!)

  NOTES
    Stirling's approximate formula is used:

      n! ~= sqrt(2*M_PI*n) * (n/M_E)^n

    Derivation of formula used for calculations is as follows:

    log2(n!) = log(n!)/log(2) = log(sqrt(2*M_PI*n)*(n/M_E)^n) / log(2) =

      = (log(2*M_PI*n)/2 + n*log(n/M_E)) / log(2).
*/

inline double log2_n_fact(double x)
{
  return (log(2*M_PI*x)/2 + x*log(x/M_E)) / M_LN2;
}


/*
  Calculate cost of merge_buffers function call for given sequence of
  input stream lengths and store the number of rows in result stream in *last.

  SYNOPSIS
    get_merge_buffers_cost()
      buff_elems  Array of #s of elements in buffers
      elem_size   Size of element stored in buffer
      first       Pointer to first merged element size
      last        Pointer to last merged element size

  RETURN
    Cost of merge_buffers operation in disk seeks.

  NOTES
    It is assumed that no rows are eliminated during merge.
    The cost is calculated as

      cost(read_and_write) + cost(merge_comparisons).

    All bytes in the sequences is read and written back during merge so cost
    of disk io is 2*elem_size*total_buf_elems/IO_SIZE (2 is for read + write)

    For comparisons cost calculations we assume that all merged sequences have
    the same length, so each of total_buf_size elements will be added to a sort
    heap with (n_buffers-1) elements. This gives the comparison cost:

      total_buf_elems* log2(n_buffers) / TIME_FOR_COMPARE_ROWID;
*/

static double get_merge_buffers_cost(uint *buff_elems, uint elem_size,
                                     uint *first, uint *last,
                                     double compare_factor)
{
  uint total_buf_elems= 0;
  for (uint *pbuf= first; pbuf <= last; pbuf++)
    total_buf_elems+= *pbuf;
  *last= total_buf_elems;

  size_t n_buffers= last - first + 1;

  /* Using log2(n)=log(n)/log(2) formula */
  return 2*((double)total_buf_elems*elem_size) / IO_SIZE +
     total_buf_elems*log((double) n_buffers) / (compare_factor * M_LN2);
}


/*
  Calculate cost of merging buffers into one in Unique::get, i.e. calculate
  how long (in terms of disk seeks) the two calls
    merge_many_buffs(...);
    merge_buffers(...);
  will take.

  SYNOPSIS
    get_merge_many_buffs_cost()
      buffer        buffer space for temporary data, at least
                    Unique::get_cost_calc_buff_size bytes
      maxbuffer     # of full buffers
      max_n_elems   # of elements in first maxbuffer buffers
      last_n_elems  # of elements in last buffer
      elem_size     size of buffer element

  NOTES
    maxbuffer+1 buffers are merged, where first maxbuffer buffers contain
    max_n_elems elements each and last buffer contains last_n_elems elements.

    The current implementation does a dumb simulation of merge_many_buffs
    function actions.

  RETURN
    Cost of merge in disk seeks.
*/

static double get_merge_many_buffs_cost(uint *buffer,
                                        uint maxbuffer, uint max_n_elems,
                                        uint last_n_elems, int elem_size,
                                        double compare_factor)
{
  int i;
  double total_cost= 0.0;
  uint *buff_elems= buffer; /* #s of elements in each of merged sequences */

  /*
    Set initial state: first maxbuffer sequences contain max_n_elems elements
    each, last sequence contains last_n_elems elements.
  */
  for (i = 0; i < (int)maxbuffer; i++)
    buff_elems[i]= max_n_elems;
  buff_elems[maxbuffer]= last_n_elems;

  /*
    Do it exactly as merge_many_buff function does, calling
    get_merge_buffers_cost to get cost of merge_buffers.
  */
  if (maxbuffer >= MERGEBUFF2)
  {
    while (maxbuffer >= MERGEBUFF2)
    {
      uint lastbuff= 0;
      for (i = 0; i <= (int) maxbuffer - MERGEBUFF*3/2; i += MERGEBUFF)
      {
        total_cost+=get_merge_buffers_cost(buff_elems, elem_size,
                                           buff_elems + i,
                                           buff_elems + i + MERGEBUFF-1,
                                           compare_factor);
	lastbuff++;
      }
      total_cost+=get_merge_buffers_cost(buff_elems, elem_size,
                                         buff_elems + i,
                                         buff_elems + maxbuffer,
                                         compare_factor);
      maxbuffer= lastbuff;
    }
  }

  /* Simulate final merge_buff call. */
  total_cost += get_merge_buffers_cost(buff_elems, elem_size,
                                       buff_elems, buff_elems + maxbuffer,
                                       compare_factor);
  return total_cost;
}


/*
  Calculate cost of using Unique for processing nkeys elements of size
  key_size using max_in_memory_size memory.

  SYNOPSIS
    Unique::get_use_cost()
      buffer    space for temporary data, use Unique::get_cost_calc_buff_size
                to get # bytes needed.
      nkeys     #of elements in Unique
      key_size  size of each elements in bytes
      max_in_memory_size   amount of memory Unique will be allowed to use
      compare_factor   used to calculate cost of one comparison
      write_fl  if the result must be saved written to disk
      in_memory_elems  OUT estimate of the number of elements in memory
                           if disk is not used  

  RETURN
    Cost in disk seeks.

  NOTES
    cost(using_unqiue) =
      cost(create_trees) +  (see #1)
      cost(merge) +         (see #2)
      cost(read_result)     (see #3)

    1. Cost of trees creation
      For each Unique::put operation there will be 2*log2(n+1) elements
      comparisons, where n runs from 1 tree_size (we assume that all added
      elements are different). Together this gives:

      n_compares = 2*(log2(2) + log2(3) + ... + log2(N+1)) = 2*log2((N+1)!)

      then cost(tree_creation) = n_compares*ROWID_COMPARE_COST;

      Total cost of creating trees:
      (n_trees - 1)*max_size_tree_cost + non_max_size_tree_cost.

      Approximate value of log2(N!) is calculated by log2_n_fact function.

    2. Cost of merging.
      If only one tree is created by Unique no merging will be necessary.
      Otherwise, we model execution of merge_many_buff function and count
      #of merges. (The reason behind this is that number of buffers is small,
      while size of buffers is big and we don't want to loose precision with
      O(x)-style formula)

    3. If only one tree is created by Unique no disk io will happen.
      Otherwise, ceil(key_len*n_keys) disk seeks are necessary. We assume
      these will be random seeks.
*/

double Unique::get_use_cost(uint *buffer, size_t nkeys, uint key_size,
                            size_t max_in_memory_size,
                            double compare_factor,
                            bool intersect_fl, bool *in_memory)
{
  size_t max_elements_in_tree;
  size_t last_tree_elems;
  size_t   n_full_trees; /* number of trees in unique - 1 */
  double result;

  max_elements_in_tree= ((size_t) max_in_memory_size /
                         ALIGN_SIZE(sizeof(TREE_ELEMENT)+key_size));

  if (max_elements_in_tree == 0)
    max_elements_in_tree= 1;

  n_full_trees=    nkeys / max_elements_in_tree;
  last_tree_elems= nkeys % max_elements_in_tree;

  /* Calculate cost of creating trees */
  result= 2*log2_n_fact(last_tree_elems + 1.0);
  if (n_full_trees)
    result+= n_full_trees * log2_n_fact(max_elements_in_tree + 1.0);
  result /= compare_factor;

  DBUG_PRINT("info",("unique trees sizes: %u=%u*%u + %u", (uint)nkeys,
                     (uint)n_full_trees, 
                     (uint)(n_full_trees?max_elements_in_tree:0),
                     (uint)last_tree_elems));

  if (in_memory)
    *in_memory= !n_full_trees;

  if (!n_full_trees)
    return result;

  /*
    There is more then one tree and merging is necessary.
    First, add cost of writing all trees to disk, assuming that all disk
    writes are sequential.
  */
  result += DISK_SEEK_BASE_COST * n_full_trees *
              ceil(((double) key_size)*max_elements_in_tree / IO_SIZE);
  result += DISK_SEEK_BASE_COST * ceil(((double) key_size)*last_tree_elems / IO_SIZE);

  /* Cost of merge */
  if (intersect_fl)
    key_size+= sizeof(element_count);
  double merge_cost= get_merge_many_buffs_cost(buffer, (uint)n_full_trees,
                                               (uint)max_elements_in_tree,
                                               (uint)last_tree_elems, key_size,
                                               compare_factor);
  result += merge_cost;
  /*
    Add cost of reading the resulting sequence, assuming there were no
    duplicate elements.
  */
  result += ceil((double)key_size*nkeys/IO_SIZE);

  return result;
}

Unique::~Unique()
{
  close_cached_file(&file);
  delete_tree(&tree, 0);
  delete_dynamic(&file_ptrs);
}


    /* Write tree to disk; clear tree */
bool Unique::flush()
{
  Merge_chunk file_ptr;
  elements+= tree.elements_in_tree;
  file_ptr.set_rowcount(tree.elements_in_tree);
  file_ptr.set_file_position(my_b_tell(&file));

  tree_walk_action action= min_dupl_count ?
		            unique_write_to_file_with_count :
		            unique_write_to_file;
  if (tree_walk(&tree, action,
		(void*) this, left_root_right) ||
      insert_dynamic(&file_ptrs, (uchar*) &file_ptr))
    return 1;
  delete_tree(&tree, 0);
  return 0;
}


/*
  Clear the tree and the file.
  You must call reset() if you want to reuse Unique after walk().
*/

void
Unique::reset()
{
  reset_tree(&tree);
  /*
    If elements != 0, some trees were stored in the file (see how
    flush() works). Note, that we can not count on my_b_tell(&file) == 0
    here, because it can return 0 right after walk(), and walk() does not
    reset any Unique member.
  */
  if (elements)
  {
    reset_dynamic(&file_ptrs);
    reinit_io_cache(&file, WRITE_CACHE, 0L, 0, 1);
  }
  my_free(sort.record_pointers);
  elements= 0;
  tree.flag= 0;
  sort.record_pointers= 0;
}

/*
  The comparison function, passed to queue_init() in merge_walk() and in
  merge_buffers() when the latter is called from Uniques::get() must
  use comparison function of Uniques::tree, but compare members of struct
  BUFFPEK.
*/

C_MODE_START

static int buffpek_compare(void *arg, const void *key_ptr1,
                           const void *key_ptr2)
{
  auto ctx= static_cast<const BUFFPEK_COMPARE_CONTEXT *>(arg);
  return ctx->key_compare(ctx->key_compare_arg,
                          *(static_cast<const uchar *const *>(key_ptr1)),
                          *(static_cast<const uchar *const *>(key_ptr2)));
}

C_MODE_END


inline
element_count get_counter_from_merged_element(void *ptr, uint ofs)
{
  element_count cnt;
  memcpy((uchar *) &cnt, (uchar *) ptr + ofs, sizeof(element_count));
  return cnt;
}


inline
void put_counter_into_merged_element(void *ptr, uint ofs, element_count cnt)
{
  memcpy((uchar *) ptr + ofs, (uchar *) &cnt, sizeof(element_count));
}


/*
  DESCRIPTION

    Function is very similar to merge_buffers, but instead of writing sorted
    unique keys to the output file, it invokes walk_action for each key.
    This saves I/O if you need to pass through all unique keys only once.

  SYNOPSIS
    merge_walk()
  All params are 'IN' (but see comment for begin, end):
    merge_buffer       buffer to perform cached piece-by-piece loading
                       of trees; initially the buffer is empty
    merge_buffer_size  size of merge_buffer. Must be aligned with
                       key_length
    key_length         size of tree element; key_length * (end - begin)
                       must be less or equal than merge_buffer_size.
    begin              pointer to BUFFPEK struct for the first tree.
    end                pointer to BUFFPEK struct for the last tree;
                       end > begin and [begin, end) form a consecutive
                       range. BUFFPEKs structs in that range are used and
                       overwritten in merge_walk().
    walk_action        element visitor. Action is called for each unique
                       key.
    walk_action_arg    argument to walk action. Passed to it on each call.
    compare            elements comparison function
    compare_arg        comparison function argument
    file               file with all trees dumped. Trees in the file
                       must contain sorted unique values. Cache must be
                       initialized in read mode.
    with counters      take into account counters for equal merged
                       elements
  RETURN VALUE
    0     ok
    <> 0  error
*/

static bool merge_walk(uchar *merge_buffer, size_t merge_buffer_size,
                       uint key_length, Merge_chunk *begin, Merge_chunk *end,
                       tree_walk_action walk_action, void *walk_action_arg,
                       qsort_cmp2 compare, void *compare_arg,
                       IO_CACHE *file, bool with_counters)
{
  BUFFPEK_COMPARE_CONTEXT compare_context = { compare, compare_arg };
  QUEUE queue;
  if (end <= begin ||
      merge_buffer_size < (size_t) (key_length * (end - begin + 1)) ||
      init_queue(&queue, (uint) (end - begin),
                 offsetof(Merge_chunk, m_current_key), 0,
                 buffpek_compare, &compare_context, 0, 0))
    return 1;
  /* we need space for one key when a piece of merge buffer is re-read */
  merge_buffer_size-= key_length;
  uchar *save_key_buff= merge_buffer + merge_buffer_size;
  uint max_key_count_per_piece= (uint) (merge_buffer_size/(end-begin) /
                                        key_length);
  /* if piece_size is aligned reuse_freed_buffer will always hit */
  uint piece_size= max_key_count_per_piece * key_length;
  ulong bytes_read;               /* to hold return value of read_to_buffer */
  Merge_chunk *top;
  int res= 1;
  uint cnt_ofs= key_length - (with_counters ? sizeof(element_count) : 0);
  element_count cnt;

  // read_to_buffer() needs only rec_length.
  Sort_param sort_param;
  sort_param.rec_length= key_length;
  DBUG_ASSERT(!sort_param.using_addon_fields());

  /*
    Invariant: queue must contain top element from each tree, until a tree
    is not completely walked through.
    Here we're forcing the invariant, inserting one element from each tree
    to the queue.
  */
  for (top= begin; top != end; ++top)
  {
    top->set_buffer(merge_buffer + (top - begin) * piece_size,
                    merge_buffer + (top - begin) * piece_size + piece_size);
    top->set_max_keys(max_key_count_per_piece);
    bytes_read= read_to_buffer(file, top, &sort_param, false);
    if (unlikely(bytes_read == (ulong) -1))
      goto end;
    DBUG_ASSERT(bytes_read);
    queue_insert(&queue, (uchar *) top);
  }
  top= (Merge_chunk *) queue_top(&queue);
  while (queue.elements > 1)
  {
    /*
      Every iteration one element is removed from the queue, and one is
      inserted by the rules of the invariant. If two adjacent elements on
      the top of the queue are not equal, biggest one is unique, because all
      elements in each tree are unique. Action is applied only to unique
      elements.
    */
    void *old_key= top->current_key();
    /*
      read next key from the cache or from the file and push it to the
      queue; this gives new top.
    */
    top->advance_current_key(key_length);
    top->decrement_mem_count();
    if (top->mem_count())
      queue_replace_top(&queue);
    else /* next piece should be read */
    {
      /* save old_key not to overwrite it in read_to_buffer */
      memcpy(save_key_buff, old_key, key_length);
      old_key= save_key_buff;
      bytes_read= read_to_buffer(file, top, &sort_param, false);
      if (unlikely(bytes_read == (ulong) -1))
        goto end;
      else if (bytes_read)      /* top->key, top->mem_count are reset */
        queue_replace_top(&queue);             /* in read_to_buffer */
      else
      {
        /*
          Tree for old 'top' element is empty: remove it from the queue and
          give all its memory to the nearest tree.
        */
        queue_remove_top(&queue);
        reuse_freed_buff(&queue, top, key_length);
      }
    }
    top= (Merge_chunk *) queue_top(&queue);
    /* new top has been obtained; if old top is unique, apply the action */
    if (compare(compare_arg, old_key, top->current_key()))
    {
      cnt= with_counters ?
           get_counter_from_merged_element(old_key, cnt_ofs) : 1;
      if (walk_action(old_key, cnt, walk_action_arg))
        goto end;
    }
    else if (with_counters)
    {
      cnt= get_counter_from_merged_element(top->current_key(), cnt_ofs);
      cnt+= get_counter_from_merged_element(old_key, cnt_ofs);
      put_counter_into_merged_element(top->current_key(), cnt_ofs, cnt);
    }
  }
  /*
    Applying walk_action to the tail of the last tree: this is safe because
    either we had only one tree in the beginning, either we work with the
    last tree in the queue.
  */
  do
  {
    do
    {
      
      cnt= with_counters ?
           get_counter_from_merged_element(top->current_key(), cnt_ofs) : 1;
      if (walk_action(top->current_key(), cnt, walk_action_arg))
        goto end;
      top->advance_current_key(key_length);
    }
    while (top->decrement_mem_count());
    bytes_read= read_to_buffer(file, top, &sort_param, false);
    if (unlikely(bytes_read == (ulong) -1))
      goto end;
  }
  while (bytes_read);
  res= 0;
end:
  delete_queue(&queue);
  return res;
}


/*
  DESCRIPTION
    Walks consecutively through all unique elements:
    if all elements are in memory, then it simply invokes 'tree_walk', else
    all flushed trees are loaded to memory piece-by-piece, pieces are
    sorted, and action is called for each unique value.
    Note: so as merging resets file_ptrs state, this method can change
    internal Unique state to undefined: if you want to reuse Unique after
    walk() you must call reset() first!
  SYNOPSIS
    Unique:walk()
  All params are 'IN':
    table   parameter for the call of the merge method
    action  function-visitor, typed in include/my_tree.h
            function is called for each unique element
    arg     argument for visitor, which is passed to it on each call
  RETURN VALUE
    0    OK
    <> 0 error
 */

bool Unique::walk(TABLE *table, tree_walk_action action, void *walk_action_arg)
{
  int res= 0;
  uchar *merge_buffer;

  if (elements == 0)                       /* the whole tree is in memory */
    return tree_walk(&tree, action, walk_action_arg, left_root_right);

  sort.return_rows= elements+tree.elements_in_tree;
  /* flush current tree to the file to have some memory for merge buffer */
  if (flush())
    return 1;
  if (flush_io_cache(&file) || reinit_io_cache(&file, READ_CACHE, 0L, 0, 0))
    return 1;
  /*
    merge_buffer must fit at least MERGEBUFF2 + 1 keys, because
    merge_index() can merge that many BUFFPEKs at once. The extra space for one key 
    is needed when a piece of merge buffer is re-read, see merge_walk()
  */
  size_t buff_sz= MY_MAX(MERGEBUFF2+1, max_in_memory_size/full_size+1) * full_size;
  if (!(merge_buffer = (uchar *)my_malloc(key_memory_Unique_merge_buffer,
                                     buff_sz, MYF(MY_THREAD_SPECIFIC|MY_WME))))
    return 1;
  if (buff_sz < full_size * (file_ptrs.elements + 1UL))
    res= merge(table, merge_buffer, buff_sz,
               buff_sz >= full_size * MERGEBUFF2) ;

  if (!res)
  {
    res= merge_walk(merge_buffer, buff_sz, full_size,
                    (Merge_chunk *) file_ptrs.buffer,
                    (Merge_chunk *) file_ptrs.buffer + file_ptrs.elements,
                    action, walk_action_arg,
                    tree.compare, tree.custom_arg, &file, with_counters);
  }
  my_free(merge_buffer);
  return res;
}


/*
  DESCRIPTION

  Perform multi-pass sort merge of the elements using the buffer buff as
  the merge buffer. The last pass is not performed if without_last_merge is
  TRUE.

  SYNOPSIS
    Unique:merge()
  All params are 'IN':
    table               the parameter to access sort context
    buff                merge buffer
    buff_size           size of merge buffer
    without_last_merge  TRUE <=> do not perform the last merge
  RETURN VALUE
    0    OK
    <> 0 error
 */

bool Unique::merge(TABLE *table, uchar *buff, size_t buff_size,
                   bool without_last_merge)
{
  IO_CACHE *outfile= &sort.io_cache;
  Merge_chunk *file_ptr= (Merge_chunk*) file_ptrs.buffer;
  uint maxbuffer= file_ptrs.elements - 1;
  my_off_t save_pos;
  bool error= 1;
  Sort_param sort_param; 

  /* Open cached file for table records if it isn't open */
  if (! my_b_inited(outfile) &&
      open_cached_file(outfile,mysql_tmpdir,TEMP_PREFIX,READ_RECORD_BUFFER,
                       MYF(MY_WME)))
    return 1;

  bzero((char*) &sort_param,sizeof(sort_param));
  sort_param.max_rows= elements;
  sort_param.sort_form= table;
  sort_param.rec_length= sort_param.sort_length= sort_param.ref_length=
   full_size;
  sort_param.min_dupl_count= min_dupl_count;
  sort_param.res_length= 0;
  sort_param.max_keys_per_buffer=
    (uint) MY_MAX((max_in_memory_size / sort_param.sort_length), MERGEBUFF2);
  sort_param.not_killable= 1;

  sort_param.unique_buff= buff +(sort_param.max_keys_per_buffer *
				       sort_param.sort_length);

  sort_param.compare= buffpek_compare;
  sort_param.cmp_context.key_compare= tree.compare;
  sort_param.cmp_context.key_compare_arg= tree.custom_arg;

  /*
    We need to remove the size allocated for the unique buffer.
    The sort_buffer_size is:
      MY_MAX(MERGEBUFF2+1, max_in_memory_size/full_size+1) * full_size;
  */
  buff_size-= full_size;

  /* Merge the buffers to one file, removing duplicates */
  if (merge_many_buff(&sort_param,
                      Bounds_checked_array<uchar>(buff, buff_size),
                      file_ptr,&maxbuffer,&file))
    goto err;
  if (flush_io_cache(&file) ||
      reinit_io_cache(&file,READ_CACHE,0L,0,0))
    goto err;
  sort_param.res_length= sort_param.rec_length-
                         (min_dupl_count ? sizeof(min_dupl_count) : 0);
  if (without_last_merge)
  {
    file_ptrs.elements= maxbuffer+1;
    return 0;
  }
  if (merge_index(&sort_param, Bounds_checked_array<uchar>(buff, buff_size),
                  file_ptr, maxbuffer, &file, outfile))
    goto err;
  error= 0;
err:
  if (flush_io_cache(outfile))
    error= 1;

  /* Setup io_cache for reading */
  save_pos= outfile->pos_in_file;
  if (reinit_io_cache(outfile,READ_CACHE,0L,0,0))
    error= 1;
  outfile->end_of_file=save_pos;
  return error;
}


/*
  Allocate memory that can be used with init_records() so that
  rows will be read in priority order.
*/

bool Unique::get(TABLE *table)
{
  bool rc= 1;
  uchar *sort_buffer= NULL;
  sort.return_rows= elements+tree.elements_in_tree;
  DBUG_ENTER("Unique::get");

  if (my_b_tell(&file) == 0)
  {
    /* Whole tree is in memory;  Don't use disk if you don't need to */
    if ((sort.record_pointers= (uchar*)
	 my_malloc(key_memory_Filesort_info_record_pointers,
                   size * tree.elements_in_tree, MYF(MY_THREAD_SPECIFIC))))
    {
      uchar *save_record_pointers= sort.record_pointers;
      tree_walk_action action= min_dupl_count ?
		          unique_intersect_write_to_ptrs :
		          unique_write_to_ptrs;
      filtered_out_elems= 0;
      (void) tree_walk(&tree, action,
		       this, left_root_right);
      /* Restore record_pointers that was changed in by 'action' above */
      sort.record_pointers= save_record_pointers;
      sort.return_rows-= filtered_out_elems;
      DBUG_RETURN(0);
    }
  }
  /* Not enough memory; Save the result to file && free memory used by tree */
  if (flush())
    DBUG_RETURN(1);
  /*
    merge_buffer must fit at least MERGEBUFF2 + 1 keys, because
    merge_index() can merge that many BUFFPEKs at once. The extra space for
    one key for Sort_param::unique_buff
  */
  size_t buff_sz= MY_MAX(MERGEBUFF2+1, max_in_memory_size/full_size+1) * full_size;

  if (!(sort_buffer= (uchar*) my_malloc(key_memory_Unique_sort_buffer, buff_sz,
                                        MYF(MY_THREAD_SPECIFIC|MY_WME))))
    DBUG_RETURN(1);

  if (merge(table, sort_buffer, buff_sz,  FALSE))
    goto err;
  rc= 0;  

err:  
  my_free(sort_buffer);  
  DBUG_RETURN(rc);
}
