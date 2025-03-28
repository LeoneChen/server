/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2018, 2020, MariaDB

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


/* Functions to handle keys and fields in forms */

#include "mariadb.h"
#include "sql_priv.h"
#include "key.h"                                // key_rec_cmp
#include "field.h"                              // Field

/*
  Search after a key that starts with 'field'

  SYNOPSIS
    find_ref_key()
    key			First key to check
    key_count		How many keys to check
    record		Start of record
    field		Field to search after
    key_length		On partial match, contains length of fields before
			field
    keypart             key part # of a field

  NOTES
   Used when calculating key for NEXT_NUMBER

  IMPLEMENTATION
    If no key starts with field test if field is part of some key. If we find
    one, then return first key and set key_length to the number of bytes
    preceding 'field'.

  RETURN
   -1  field is not part of the key
   #   Key part for key matching key.
       key_length is set to length of key before (not including) field
*/

int find_ref_key(KEY *key, uint key_count, uchar *record, Field *field,
                 uint *key_length, uint *keypart)
{
  int i;
  KEY *key_info;
  uint fieldpos;

  fieldpos= field->offset(record);

  /* Test if some key starts as fieldpos */
  for (i= 0, key_info= key ;
       i < (int) key_count ;
       i++, key_info++)
  {
    if (key_info->key_part[0].offset == fieldpos &&
            key_info->key_part[0].field->type() != MYSQL_TYPE_BIT)
    {                                  		/* Found key. Calc keylength */
      *key_length= *keypart= 0;
      return i;                                 /* Use this key */
    }
  }

  /* Test if some key contains fieldpos */
  for (i= 0, key_info= key;
       i < (int) key_count ;
       i++, key_info++)
  {
    uint j;
    KEY_PART_INFO *key_part;
    *key_length=0;
    for (j=0, key_part=key_info->key_part ;
	 j < key_info->user_defined_key_parts ;
	 j++, key_part++)
    {
      if (key_part->offset == fieldpos &&
            key_part->field->type() != MYSQL_TYPE_BIT)
      {
        *keypart= j;
        return i;                               /* Use this key */
      }
      *key_length+= key_part->store_length;
    }
  }
  return(-1);					/* No key is ok */
}


/**
  Copy part of a record that forms a key or key prefix to a buffer.

    The function takes a complete table record (as e.g. retrieved by
    handler::index_read()), and a description of an index on the same table,
    and extracts the first key_length bytes of the record which are part of a
    key into to_key. If length == 0 then copy all bytes from the record that
    form a key.

  @param to_key      buffer that will be used as a key
  @param from_record full record to be copied from
  @param key_info    descriptor of the index
  @param key_length  specifies length of all keyparts that will be copied
  @param with_zerofill  skipped bytes in the key buffer to be filled with 0
*/

void key_copy(uchar *to_key, const uchar *from_record, const KEY *key_info,
              uint key_length, bool with_zerofill)
{
  uint length;
  KEY_PART_INFO *key_part;

  if (key_length == 0)
    key_length= key_info->key_length;
  for (key_part= key_info->key_part;
       (int) key_length > 0;
       key_part++, to_key+= length, key_length-= length)
  {
    if (key_part->null_bit)
    {
      *to_key++= MY_TEST(from_record[key_part->null_offset] &
                         key_part->null_bit);
      key_length--;
      if (to_key[-1])
      {
        /*
          Don't copy data for null values
          The -1 below is to subtract the null byte which is already handled
        */
        length= MY_MIN(key_length, uint(key_part->store_length)-1);
        if (with_zerofill)
          bzero((char*) to_key, length);
        continue;
      }
    }
    auto *from_ptr= key_part->field->ptr_in_record(from_record);
    if (key_part->key_part_flag & HA_BLOB_PART ||
        key_part->key_part_flag & HA_VAR_LENGTH_PART)
    {
      key_length-= HA_KEY_BLOB_LENGTH;
      length= MY_MIN(key_length, key_part->length);
      uint bytes= key_part->field->get_key_image(to_key, length, from_ptr,
		      key_info->flags & HA_SPATIAL ? Field::itMBR : Field::itRAW);
      if (with_zerofill && bytes < length)
        bzero((char*) to_key + bytes, length - bytes);
      to_key+= HA_KEY_BLOB_LENGTH;
    }
    else
    {
      length= MY_MIN(key_length, key_part->length);
      Field *field= key_part->field;
      CHARSET_INFO *cs= field->charset();
      uint bytes= field->get_key_image(to_key, length, from_ptr, Field::itRAW);
      if (bytes < length)
        cs->fill((char*) to_key + bytes, length - bytes, ' ');
    }
  }
}


/**
  Restore a key from some buffer to record.

    This function converts a key into record format. It can be used in cases
    when we want to return a key as a result row.

  @param to_record   record buffer where the key will be restored to
  @param from_key    buffer that contains a key
  @param key_info    descriptor of the index
  @param key_length  specifies length of all keyparts that will be restored
*/

void key_restore(uchar *to_record, const uchar *from_key, KEY *key_info,
                 uint key_length)
{
  uint length;
  KEY_PART_INFO *key_part;

  if (key_length == 0)
  {
    key_length= key_info->key_length;
  }
  for (key_part= key_info->key_part ;
       (int) key_length > 0 ;
       key_part++, from_key+= length, key_length-= length)
  {
    uchar used_uneven_bits= 0;
    if (key_part->null_bit)
    {
      bool null_value; 
      if ((null_value= *from_key++))
	to_record[key_part->null_offset]|= key_part->null_bit;
      else
	to_record[key_part->null_offset]&= ~key_part->null_bit;
      key_length--;
      if (null_value)
      {
        /*
          Don't copy data for null bytes
          The -1 below is to subtract the null byte which is already handled
        */
        length= MY_MIN(key_length, uint(key_part->store_length)-1);
        continue;
      }
    }
    if (key_part->type == HA_KEYTYPE_BIT)
    {
      Field_bit *field= (Field_bit *) (key_part->field);
      if (field->bit_len)
      {
        uchar bits= *(from_key + key_part->length -
                      field->pack_length_in_rec() - 1);
        set_rec_bits(bits, to_record + key_part->null_offset +
                     (key_part->null_bit == 128),
                     field->bit_ofs, field->bit_len);
        /* we have now used the byte with 'uneven' bits */
        used_uneven_bits= 1;
      }
    }
    if (key_part->key_part_flag & HA_BLOB_PART)
    {
      /*
        This in fact never happens, as we have only partial BLOB
        keys yet anyway, so it's difficult to find any sense to
        restore the part of a record.
        Maybe this branch is to be removed, but now we
        have to ignore GCov compaining.
      */
      uint blob_length= uint2korr(from_key);
      Field_blob *field= (Field_blob*) key_part->field;
      from_key+= HA_KEY_BLOB_LENGTH;
      key_length-= HA_KEY_BLOB_LENGTH;
      field->set_ptr_offset(to_record - field->table->record[0],
                            (ulong) blob_length, from_key);
      length= key_part->length;
    }
    else if (key_part->key_part_flag & HA_VAR_LENGTH_PART)
    {
      Field *field= key_part->field;
      my_ptrdiff_t ptrdiff= to_record - field->table->record[0];
      field->move_field_offset(ptrdiff);
      key_length-= HA_KEY_BLOB_LENGTH;
      length= MY_MIN(key_length, key_part->length);
      MY_BITMAP *old_map= dbug_tmp_use_all_columns(field->table, &field->table->write_set);
      field->set_key_image(from_key, length);
      dbug_tmp_restore_column_map(&field->table->write_set, old_map);
      from_key+= HA_KEY_BLOB_LENGTH;
      field->move_field_offset(-ptrdiff);
    }
    else
    {
      length= MY_MIN(key_length, key_part->length);
      /* skip the byte with 'uneven' bits, if used */
      memcpy(to_record + key_part->offset, from_key + used_uneven_bits
             , (size_t) length - used_uneven_bits);
    }
  }
}


/**
  Compare if a key has changed.

  @param table		TABLE
  @param key		key to compare to row
  @param idx		Index used
  @param key_length	Length of key

  @note
    In theory we could just call field->cmp() for all field types,
    but as we are only interested if a key has changed (not if the key is
    larger or smaller than the previous value) we can do things a bit
    faster by using memcmp() instead.

  @retval
    0	If key is equal
  @retval
    1	Key has changed
*/

bool key_cmp_if_same(TABLE *table,const uchar *key,uint idx,uint key_length)
{
  uint store_length;
  KEY_PART_INFO *key_part;
  const uchar *key_end= key + key_length;;

  for (key_part=table->key_info[idx].key_part;
       key < key_end ; 
       key_part++, key+= store_length)
  {
    uint length;
    store_length= key_part->store_length;

    if (key_part->null_bit)
    {
      if (*key != MY_TEST(table->record[0][key_part->null_offset] &
                          key_part->null_bit))
	return 1;
      if (*key)
	continue;
      key++;
      store_length--;
    }
    if (!(key_part->key_part_flag & HA_CAN_MEMCMP))
    {
      if (key_part->field->key_cmp(key, key_part->length))
	return 1;
      continue;
    }
    length= MY_MIN((uint) (key_end-key), store_length);
    if (!(key_part->key_type & (FIELDFLAG_NUMBER+FIELDFLAG_BINARY+
                                FIELDFLAG_PACK)))
    {
      CHARSET_INFO *cs= key_part->field->charset();
      size_t char_length= key_part->length / cs->mbmaxlen;
      const uchar *pos= table->record[0] + key_part->offset;
      if (length > char_length)
      {
        char_length= cs->charpos(pos, pos + length, char_length);
        set_if_smaller(char_length, length);
      }
      if (cs->strnncollsp(key, length, pos, char_length))
        return 1;
      continue;
    }
    if (memcmp(key,table->record[0]+key_part->offset,length))
      return 1;
  }
  return 0;
}


/**
  Unpack a field and append it.

  @param[inout] to           String to append the field contents to.
  @param        field        Field to unpack.
  @param        rec          Record which contains the field data.
  @param        max_length   Maximum length of field to unpack
                             or 0 for unlimited.
  @param        prefix_key   The field is used as a prefix key.
*/

void field_unpack(String *to, Field *field, const uchar *rec, uint max_length,
                  bool prefix_key)
{
  String tmp;
  DBUG_ENTER("field_unpack");
  if (!max_length)
    max_length= field->pack_length();
  if (field)
  {
    if (field->is_null())
    {
      to->append(NULL_clex_str);
      DBUG_VOID_RETURN;
    }
    CHARSET_INFO *cs= field->charset();
    field->val_str(&tmp);
    /*
      For BINARY(N) strip trailing zeroes to make
      the error message nice-looking
    */
    if (field->binary() &&  field->type() == MYSQL_TYPE_STRING && tmp.length())
    {
      const char *tmp_end= tmp.ptr() + tmp.length();
      while (tmp_end > tmp.ptr() && !*--tmp_end) ;
      tmp.length((uint32)(tmp_end - tmp.ptr() + 1));
    }
    if (cs->mbmaxlen > 1 && prefix_key)
    {
      /*
        Prefix key, multi-byte charset.
        For the columns of type CHAR(N), the above val_str()
        call will return exactly "key_part->length" bytes,
        which can break a multi-byte characters in the middle.
        Align, returning not more than "char_length" characters.
      */
      size_t charpos, char_length= max_length / cs->mbmaxlen;
      if ((charpos= cs->charpos(tmp.ptr(),
                                tmp.ptr() + tmp.length(),
                                char_length)) < tmp.length())
        tmp.length(charpos);
    }
    if (max_length < field->pack_length())
      tmp.length(MY_MIN(tmp.length(),max_length));
    ErrConvString err(&tmp);
    to->append(err.lex_cstring());
  }
  else
    to->append(STRING_WITH_LEN("???"));
  DBUG_VOID_RETURN;
}


/*
  unpack key-fields from record to some buffer.

  This is used mainly to get a good error message.  We temporary 
  change the column bitmap so that all columns are readable.

  @param
     to		Store value here in an easy to read form
  @param
     table	Table to use
  @param
     key	Key
*/

void key_unpack(String *to, TABLE *table, KEY *key)
{
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  DBUG_ENTER("key_unpack");

  to->length(0);
  KEY_PART_INFO *key_part_end= key->key_part + key->user_defined_key_parts;
  for (KEY_PART_INFO *key_part= key->key_part;
       key_part < key_part_end;
       key_part++)
  {
    if (key_part->field->invisible > INVISIBLE_USER)
      continue;
    if (to->length())
      to->append('-');
    if (key_part->null_bit)
    {
      if (table->record[0][key_part->null_offset] & key_part->null_bit)
      {
	to->append(NULL_clex_str);
	continue;
      }
    }
    field_unpack(to, key_part->field, table->record[0], key_part->length,
                 MY_TEST(key_part->key_part_flag & HA_PART_KEY_SEG));
 }
  dbug_tmp_restore_column_map(&table->read_set, old_map);
  DBUG_VOID_RETURN;
}


/*
  Check if key uses field that is marked in passed field bitmap.

  SYNOPSIS
    is_key_used()
      table   TABLE object with which keys and fields are associated.
      idx     Key to be checked.
      fields  Bitmap of fields to be checked.

  NOTE
    This function uses TABLE::tmp_set bitmap so the caller should care
    about saving/restoring its state if it also uses this bitmap.

  RETURN VALUE
    TRUE   Key uses field from bitmap
    FALSE  Otherwise
*/

bool is_key_used(TABLE *table, uint idx, const MY_BITMAP *fields)
{
  table->mark_index_columns(idx, &table->tmp_set);
  return bitmap_is_overlapping(&table->tmp_set, fields);
}


/**
  Compare key in row to a given key.

  @param key_part		Key part handler
  @param key			Key to compare to value in table->record[0]
  @param key_length		length of 'key'

  @return
    The return value is SIGN(key_in_row - range_key):
    -   0		Key is equal to range or 'range' == 0 (no range)
    -  -1		Key is less than range
    -   1		Key is larger than range
*/

int key_cmp(KEY_PART_INFO *key_part, const uchar *key, uint key_length)
{
  uint store_length;

  for (const uchar *end=key + key_length;
       key < end;
       key+= store_length, key_part++)
  {
    int cmp;
    store_length= key_part->store_length;
    if (key_part->null_bit)
    {
      /* This key part allows null values; NULL is lower than everything */
      bool field_is_null= key_part->field->is_null();
      if (*key)                                 // If range key is null
      {
	/* the range is expecting a null value */
	if (!field_is_null)
	  return 1;                             // Found key is > range
        /* null -- exact match, go to next key part */
	continue;
      }
      else if (field_is_null)
	return -1;                              // NULL is less than any value
      key++;					// Skip null byte
      store_length--;
    }
    if ((cmp=key_part->field->key_cmp(key, key_part->length)) < 0)
      return -1;
    if (cmp > 0)
      return 1;
  }
  return 0;                                     // Keys are equal
}


/**
  Compare two records in index order.

  This method is set-up such that it can be called directly from the
  priority queue and it is attempted to be optimised as much as possible
  since this will be called O(N * log N) times while performing a merge
  sort in various places in the code.

  We retrieve the pointer to table->record[0] using the fact that key_parts
  have an offset making it possible to calculate the start of the record.
  We need to get the diff to the compared record since none of the records
  being compared are stored in table->record[0].

  We first check for NULL values, if there are no NULL values we use
  a compare method that gets two field pointers and a max length
  and return the result of the comparison.

  key is a null terminated array, since in some cases (clustered
  primary key) it must compare more than one index.

  @param key                    Null terminated array of index information
  @param first_rec              Pointer to record compare with
  @param second_rec             Pointer to record compare against first_rec

  @return Return value is SIGN(first_rec - second_rec)
    @retval  0                  Keys are equal
    @retval -1                  second_rec is greater than first_rec
    @retval +1                  first_rec is greater than second_rec
*/

int key_rec_cmp(const KEY *const *key, const uchar *first_rec,
                const uchar *second_rec)
{
  const KEY *key_info= *(key++);                     // Start with first key
  uint key_parts, key_part_num;
  KEY_PART_INFO *key_part= key_info->key_part;
  uchar *rec0= key_part->field->ptr - key_part->offset;
  my_ptrdiff_t first_diff= first_rec - rec0, sec_diff= second_rec - rec0;
  int result= 0;
  Field *field;
  DBUG_ENTER("key_rec_cmp");

  /* loop over all given keys */
  do
  {
    key_parts= key_info->user_defined_key_parts;
    key_part= key_info->key_part;
    key_part_num= 0;

    /* loop over every key part */
    do
    {
      field= key_part->field;

      if (key_part->null_bit)
      {
        /* The key_part can contain NULL values */
        bool first_is_null= field->is_real_null(first_diff);
        bool sec_is_null= field->is_real_null(sec_diff);
        /*
          NULL is smaller then everything so if first is NULL and the other
          not then we know that we should return -1 and for the opposite
          we should return +1. If both are NULL then we call it equality
          although it is a strange form of equality, we have equally little
          information of the real value.
        */
        if (!first_is_null)
        {
          if (!sec_is_null)
            ; /* Fall through, no NULL fields */
          else
          {
            DBUG_RETURN(+1);
          }
        }
        else if (!sec_is_null)
        {
          DBUG_RETURN(-1);
        }
        else
          goto next_loop; /* Both were NULL */
      }
      /*
        No null values in the fields
        We use the virtual method cmp_prefix with a max length parameter.
        For most field types this translates into a cmp without
        max length. The exceptions are the BLOB and VARCHAR field types
        that take the max length into account.
      */
      if ((result= field->cmp_prefix(field->ptr+first_diff, field->ptr+sec_diff,
                                     key_part->length /
                                     field->charset()->mbmaxlen)))
        DBUG_RETURN(result);
next_loop:
      key_part++;
      key_part_num++;
    } while (key_part_num < key_parts); /* this key is done */

    key_info= *(key++);
  } while (key_info); /* no more keys to test */
  DBUG_RETURN(0);
}


/*
  Compare two key tuples.

  @brief
    Compare two key tuples, i.e. two key values in KeyTupleFormat.

  @param part          KEY_PART_INFO with key description
  @param key1          First key to compare
  @param key2          Second key to compare 
  @param tuple_length  Length of key1 (and key2, they are the same) in bytes.

  @return
    @retval  0  key1 == key2
    @retval -1  key1 < key2
    @retval +1  key1 > key2 
*/

int key_tuple_cmp(KEY_PART_INFO *part, const uchar *key1, const uchar *key2,
                  uint tuple_length)
{
  const uchar *key1_end= key1 + tuple_length;
  int UNINIT_VAR(len);
  int res;
  for (;key1 < key1_end; key1 += len, key2 += len, part++)
  {
    len= part->store_length;
    if (part->null_bit)
    {
      if (*key1) // key1 == NULL
      {
        if (!*key2) // key1(NULL) < key2(notNULL)
          return -1;
        continue;
      }
      else if (*key2) // key1(notNULL) > key2 (NULL)
        return 1;
      /* Step over the NULL bytes for key_cmp() call */
      key1++;
      key2++;
      len--;
    }
    if ((res= part->field->key_cmp(key1, key2)))
      return res;
  }
  return 0;
}

/**
  Get hash value for the key from a key buffer 

  @param  key_info       the key descriptor
  @param  used_key_part  number of key parts used for the key
  @param  key            pointer to the buffer with the key value

  @datails
  When hashing we should take special care only of:
  1. NULLs (and keyparts which can be null so one byte reserved for it);
  2. Strings for which we have to take into account their collations
  and the values of their lengths in the prefixes.

  @return  hash value calculated for the key
*/

ulong key_hashnr(KEY *key_info, uint used_key_parts, const uchar *key)
{
  ulong nr=1, nr2=4;
  KEY_PART_INFO *key_part= key_info->key_part;
  KEY_PART_INFO *end_key_part= key_part + used_key_parts;

  for (; key_part < end_key_part; key_part++)
  {
    uchar *pos= (uchar*)key;
    CHARSET_INFO *UNINIT_VAR(cs);
    size_t UNINIT_VAR(length), UNINIT_VAR(pack_length);
    bool is_string= TRUE;

    key+= key_part->length;
    if (key_part->null_bit)
    {
      key++;                       /* Skip null byte */
      if (*pos)                    /* Found null */
      {
        nr^= (nr << 1) | 1;
        /* Add key pack length to key for VARCHAR segments */
        switch (key_part->type) {
        case HA_KEYTYPE_VARTEXT1:
        case HA_KEYTYPE_VARBINARY1:
        case HA_KEYTYPE_VARTEXT2:
        case HA_KEYTYPE_VARBINARY2:
          key+= 2;
          break;
        default:
          ;
        }
    continue;
      }
      pos++;                       /* Skip null byte */
    }
    /* If it is string set parameters of the string */
    switch (key_part->type) {
    case HA_KEYTYPE_TEXT:
      cs= key_part->field->charset();
      length= key_part->length;
      pack_length= 0;
      break;
    case HA_KEYTYPE_BINARY :
      cs= &my_charset_bin;
      length= key_part->length;
      pack_length= 0;
      break;
    case HA_KEYTYPE_VARTEXT1:
    case HA_KEYTYPE_VARTEXT2:
      cs= key_part->field->charset();
      length= uint2korr(pos);
      pack_length= 2;
      break;
    case HA_KEYTYPE_VARBINARY1:
    case HA_KEYTYPE_VARBINARY2:
      cs= &my_charset_bin;
      length= uint2korr(pos);
      pack_length= 2;
      break;
    default:
      is_string= FALSE;
    }

    if (is_string)
    {
      /*
        Surprisingly, BNL-H joins may use prefix keys. This may happen
        when there is a real index on the column used in equi-join.

        In this case, the passed key tuple is already a prefix, no
        special handling is required.
      */
      cs->hash_sort(pos+pack_length, length, &nr, &nr2);
      key+= pack_length;
    }
    else
    {
      for (; pos < (uchar*)key ; pos++)
      {
        nr^=(ulong) ((((uint) nr & 63)+nr2)*((uint) *pos)) + (nr << 8);
        nr2+=3;
      }
    }
  }
  DBUG_PRINT("exit", ("hash: %lx", nr));
  return(nr);
}


/**
  Check whether two keys in the key buffers are equal

  @param key_info        the key descriptor
  @param  used_key_part  number of key parts used for the keys
  @param key1            pointer to the buffer with the first key 
  @param key2            pointer to the buffer with the second key 

  @detail See details of key_hashnr().

  @retval TRUE  keys in the buffers are NOT equal
  @retval FALSE keys in the buffers are equal
*/

bool key_buf_cmp(KEY *key_info, uint used_key_parts,
                 const uchar *key1, const uchar *key2)
{
  KEY_PART_INFO *key_part= key_info->key_part;
  KEY_PART_INFO *end_key_part= key_part + used_key_parts;

  for (; key_part < end_key_part; key_part++)
  {
    uchar *pos1= (uchar*)key1;
    uchar *pos2= (uchar*)key2;
    CHARSET_INFO *UNINIT_VAR(cs);
    size_t UNINIT_VAR(length1), UNINIT_VAR(length2), UNINIT_VAR(pack_length);
    bool is_string= TRUE;

    key1+= key_part->length;
    key2+= key_part->length;
    if (key_part->null_bit)
    {
      key1++; key2++;                           /* Skip null byte */
      if (*pos1 && *pos2)                       /* Both are null */
      {
        /* Add key pack length to key for VARCHAR segments */
        switch (key_part->type) {
        case HA_KEYTYPE_VARTEXT1:
        case HA_KEYTYPE_VARBINARY1:
        case HA_KEYTYPE_VARTEXT2:
        case HA_KEYTYPE_VARBINARY2:
          key1+= 2; key2+= 2;
          break;
        default:
          ;
        }
        continue;
      }
      if (*pos1 != *pos2)
        return TRUE;
      pos1++; pos2++;
    }

    /* If it is string set parameters of the string */
    switch (key_part->type) {
    case HA_KEYTYPE_TEXT:
      cs= key_part->field->charset();
      length1= length2= key_part->length;
      pack_length= 0;
      break;
    case HA_KEYTYPE_BINARY :
      cs= &my_charset_bin;
      length1= length2= key_part->length;
      pack_length= 0;
      break;
    case HA_KEYTYPE_VARTEXT1:
    case HA_KEYTYPE_VARTEXT2:
      cs= key_part->field->charset();
      length1= uint2korr(pos1);
      length2= uint2korr(pos2);
      pack_length= 2;
      break;
    case HA_KEYTYPE_VARBINARY1:
    case HA_KEYTYPE_VARBINARY2:
      cs= &my_charset_bin;
      length1= uint2korr(pos1);
      length2= uint2korr(pos2);
      pack_length= 2;
      break;
    default:
      is_string= FALSE;
    }

    if (is_string)
    {
      /*
        Surprisingly, BNL-H joins may use prefix keys. This may happen
        when there is a real index on the column used in equi-join.
        In this case, we get properly truncated prefixes here.
      */
      if (cs->strnncollsp(pos1 + pack_length, length1,
                          pos2 + pack_length, length2))
        return true;
      key1+= pack_length; key2+= pack_length;
    }
    else
    {
      /* it is OK to compare non-string byte per byte */
      for (; pos1 < (uchar*)key1 ; pos1++, pos2++)
      {
        if (pos1[0] != pos2[0])
          return TRUE;
      }
    }
  }
  return FALSE;
}
