/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All rights reserved.
Copyright (c) 2009, Percona Inc.
Copyright (c) 2013, 2022, MariaDB Corporation.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/********************************************************************//**
@file srv/srv0start.cc
Starts the InnoDB database server

Created 2/16/1996 Heikki Tuuri
*************************************************************************/

#include "my_global.h"

#include "mysqld.h"
#include "mysql/psi/mysql_stage.h"
#include "mysql/psi/psi.h"

#include "row0ftsort.h"
#include "ut0mem.h"
#include "mem0mem.h"
#include "data0data.h"
#include "data0type.h"
#include "dict0dict.h"
#include "buf0buf.h"
#include "buf0dblwr.h"
#include "buf0dump.h"
#include "os0file.h"
#include "fil0fil.h"
#include "fil0crypt.h"
#include "fsp0fsp.h"
#include "rem0rec.h"
#include "mtr0mtr.h"
#include "log0crypt.h"
#include "log0recv.h"
#include "page0page.h"
#include "page0cur.h"
#include "trx0trx.h"
#include "trx0sys.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "rem0rec.h"
#include "ibuf0ibuf.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "btr0defragment.h"
#include "mysql/service_wsrep.h" /* wsrep_recovery */
#include "trx0rseg.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "dict0boot.h"
#include "dict0load.h"
#include "dict0stats_bg.h"
#include "que0que.h"
#include "lock0lock.h"
#include "trx0roll.h"
#include "trx0purge.h"
#include "lock0lock.h"
#include "pars0pars.h"
#include "btr0sea.h"
#include "rem0cmp.h"
#include "dict0crea.h"
#include "row0ins.h"
#include "row0sel.h"
#include "row0upd.h"
#include "row0row.h"
#include "row0mysql.h"
#include "btr0pcur.h"
#include "zlib.h"
#include "ut0crc32.h"
#include "log.h"

/** Log sequence number at shutdown */
lsn_t	srv_shutdown_lsn;

/** TRUE if a raw partition is in use */
ibool	srv_start_raw_disk_in_use;

/** UNDO tablespaces starts with space id. */
ulint	srv_undo_space_id_start;

/** TRUE if the server is being started, before rolling back any
incomplete transactions */
bool	srv_startup_is_before_trx_rollback_phase;
/** TRUE if the server is being started */
bool	srv_is_being_started;
/** TRUE if the server was successfully started */
bool	srv_was_started;
/** The original value of srv_log_file_size (innodb_log_file_size) */
static ulonglong	srv_log_file_size_requested;
/** whether srv_start() has been called */
static bool		srv_start_has_been_called;

/** Whether any undo log records can be generated */
bool	srv_undo_sources;

#ifdef UNIV_DEBUG
/** InnoDB system tablespace to set during recovery */
uint	srv_sys_space_size_debug;
/** whether redo log file have been created at startup */
bool	srv_log_file_created;
#endif /* UNIV_DEBUG */

/** whether some background threads that create redo log have been started */
static bool srv_started_redo;

/** At a shutdown this value climbs from SRV_SHUTDOWN_NONE to
SRV_SHUTDOWN_CLEANUP and then to SRV_SHUTDOWN_LAST_PHASE, and so on */
enum srv_shutdown_t	srv_shutdown_state = SRV_SHUTDOWN_NONE;

/** Name of srv_monitor_file */
static char*	srv_monitor_file_name;
std::unique_ptr<tpool::timer> srv_master_timer;

/** */
#define SRV_MAX_N_PENDING_SYNC_IOS	100

#ifdef UNIV_PFS_THREAD
/* Keys to register InnoDB threads with performance schema */
mysql_pfs_key_t	thread_pool_thread_key;
#endif /* UNIV_PFS_THREAD */

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Array of all InnoDB stage events for monitoring activities via
performance schema. */
static PSI_stage_info*	srv_stages[] =
{
	&srv_stage_alter_table_end,
	&srv_stage_alter_table_insert,
	&srv_stage_alter_table_log_index,
	&srv_stage_alter_table_log_table,
	&srv_stage_alter_table_merge_sort,
	&srv_stage_alter_table_read_pk_internal_sort,
	&srv_stage_buffer_pool_load,
};
#endif /* HAVE_PSI_STAGE_INTERFACE */

/*********************************************************************//**
Check if a file can be opened in read-write mode.
@return true if it doesn't exist or can be opened in rw mode. */
static
bool
srv_file_check_mode(
/*================*/
	const char*	name)		/*!< in: filename to check */
{
	os_file_stat_t	stat;

	memset(&stat, 0x0, sizeof(stat));

	dberr_t		err = os_file_get_status(
		name, &stat, true, srv_read_only_mode);

	if (err == DB_FAIL) {
		ib::error() << "os_file_get_status() failed on '" << name
			<< "'. Can't determine file permissions.";
		return(false);

	} else if (err == DB_SUCCESS) {

		/* Note: stat.rw_perm is only valid of files */

		if (stat.type == OS_FILE_TYPE_FILE) {

			if (!stat.rw_perm) {
				const char*	mode = srv_read_only_mode
					? "read" : "read-write";
				ib::error() << name << " can't be opened in "
					<< mode << " mode.";
				return(false);
			}
		} else {
			/* Not a regular file, bail out. */
			ib::error() << "'" << name << "' not a regular file.";

			return(false);
		}
	} else {

		/* This is OK. If the file create fails on RO media, there
		is nothing we can do. */

		ut_a(err == DB_NOT_FOUND);
	}

	return(true);
}

/** Initial number of the redo log file */
static const char INIT_LOG_FILE0[]= "101";

/** Creates log file.
@param[in]  create_new_db   whether the database is being initialized
@param[in]  lsn		    FIL_PAGE_FILE_FLUSH_LSN value
@param[out] logfile0        name of the log file
@return DB_SUCCESS or error code */
static dberr_t create_log_file(bool create_new_db, lsn_t lsn,
                               std::string& logfile0)
{
	if (srv_read_only_mode) {
		ib::error() << "Cannot create log file in read-only mode";
		return DB_READ_ONLY;
	}

	if (!log_set_capacity(srv_log_file_size_requested)) {
		return(DB_ERROR);
	}

	/* Crashing after deleting the first file should be
	recoverable. The buffer pool was clean, and we can simply
	create log file from the scratch. */
	DBUG_EXECUTE_IF("innodb_log_abort_6", delete_log_file("0");
			return DB_ERROR;);

	for (size_t i = 0; i < 102; i++) {
		delete_log_file(std::to_string(i).c_str());
	}

	DBUG_PRINT("ib_log", ("After innodb_log_abort_6"));
	ut_ad(!os_aio_pending_reads());
	ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
	ut_ad(!buf_pool.get_oldest_modification(0));
	ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));
	/* os_aio_pending_writes() may hold here if some
	write_io_callback() did not release the slot yet.  However,
	the page write itself must have completed, because the
	buf_pool.flush_list is empty. In debug builds, we wait for
	this to happen, hoping to get a hung process if this
	assumption does not hold. */
	ut_d(os_aio_wait_until_no_pending_writes(false));

	DBUG_EXECUTE_IF("innodb_log_abort_7", return DB_ERROR;);
	DBUG_PRINT("ib_log", ("After innodb_log_abort_7"));

	logfile0 = get_log_file_path(LOG_FILE_NAME_PREFIX)
			   .append(INIT_LOG_FILE0);

	bool ret;
	pfs_os_file_t file = os_file_create(
		innodb_log_file_key, logfile0.c_str(),
		OS_FILE_CREATE|OS_FILE_ON_ERROR_NO_EXIT,
		OS_LOG_FILE, srv_read_only_mode, &ret);

	if (!ret) {
		ib::error() << "Cannot create " << logfile0;
		return DB_ERROR;
	}

	ib::info() << "Setting log file " << logfile0 << " size to "
		   << srv_log_file_size << " bytes";

	ret = os_file_set_size(logfile0.c_str(), file, srv_log_file_size);
	if (!ret) {
		os_file_close(file);
		ib::error() << "Cannot set log file " << logfile0
			    << " size to " << srv_log_file_size << " bytes";
		return DB_ERROR;
	}

	ret = os_file_close(file);
	ut_a(ret);

	DBUG_EXECUTE_IF("innodb_log_abort_8", return(DB_ERROR););
	DBUG_PRINT("ib_log", ("After innodb_log_abort_8"));

	/* We did not create the first log file initially as LOG_FILE_NAME, so
	that crash recovery cannot find it until it has been completed and
        renamed. */

	log_sys.log.create();

	log_sys.log.open_file(logfile0);

	/* Create a log checkpoint. */
	mysql_mutex_lock(&log_sys.mutex);
	if (log_sys.is_encrypted() && !log_crypt_init()) {
		return DB_ERROR;
	}
	ut_d(recv_no_log_write = false);
	lsn = ut_uint64_align_up(lsn, OS_FILE_LOG_BLOCK_SIZE);
	log_sys.set_lsn(lsn + LOG_BLOCK_HDR_SIZE);
	log_sys.log.set_lsn(lsn);
	log_sys.log.set_lsn_offset(LOG_FILE_HDR_SIZE);

	log_sys.buf_next_to_write = 0;
	log_sys.write_lsn = lsn;

	log_sys.next_checkpoint_no = 0;
	log_sys.last_checkpoint_lsn = 0;

	memset(log_sys.buf, 0, srv_log_buffer_size);
	log_block_init(log_sys.buf, lsn);
	log_block_set_first_rec_group(log_sys.buf, LOG_BLOCK_HDR_SIZE);
	memset(log_sys.flush_buf, 0, srv_log_buffer_size);

	log_sys.buf_free = LOG_BLOCK_HDR_SIZE;

	log_sys.log.write_header_durable(lsn);

	ut_ad(srv_startup_is_before_trx_rollback_phase);
	if (create_new_db) {
		srv_startup_is_before_trx_rollback_phase = false;
	}

	/* After disabling recv_no_log_write, enable checkpoints
	in buf_flush_page_cleaner(). This could help to avoid
	crash during log file resizing */
	recv_sys.recovery_on = false;

	mysql_mutex_unlock(&log_sys.mutex);

	log_make_checkpoint();
	log_buffer_flush_to_disk();

	return DB_SUCCESS;
}

/** Rename the first redo log file.
@param[in]	lsn		FIL_PAGE_FILE_FLUSH_LSN value
@param[in,out]	logfile0	name of the first log file
@return	error code
@retval	DB_SUCCESS	on successful operation */
MY_ATTRIBUTE((warn_unused_result))
static dberr_t create_log_file_rename(lsn_t lsn, std::string &logfile0)
{
  ut_ad(!srv_log_file_created);
  ut_d(srv_log_file_created= true);

  DBUG_EXECUTE_IF("innodb_log_abort_9", return (DB_ERROR););
  DBUG_PRINT("ib_log", ("After innodb_log_abort_9"));

  /* Rename the first log file, now that a log checkpoint has been created. */
  auto new_name = get_log_file_path();

  ib::info() << "Renaming log file " << logfile0 << " to " << new_name;

  mysql_mutex_lock(&log_sys.mutex);
  ut_ad(logfile0.size() == 2 + new_name.size());
  logfile0= new_name;
  dberr_t err= log_sys.log.rename(std::move(new_name));

  mysql_mutex_unlock(&log_sys.mutex);

  DBUG_EXECUTE_IF("innodb_log_abort_10", err= DB_ERROR;);

  if (err == DB_SUCCESS)
    ib::info() << "New log file created, LSN=" << lsn;

  return err;
}

/** Create an undo tablespace file
@param[in] name  file name
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespace_create(const char* name)
{
	pfs_os_file_t	fh;
	bool		ret;
	dberr_t		err = DB_SUCCESS;

	os_file_create_subdirs_if_needed(name);

	fh = os_file_create(
		innodb_data_file_key,
		name,
		srv_read_only_mode ? OS_FILE_OPEN : OS_FILE_CREATE,
		OS_DATA_FILE, srv_read_only_mode, &ret);

	if (!ret) {
		if (os_file_get_last_error(false) != OS_FILE_ALREADY_EXISTS
#ifdef _AIX
			/* AIX 5.1 after security patch ML7 may have
			errno set to 0 here, which causes our function
			to return 100; work around that AIX problem */
		    && os_file_get_last_error(false) != 100
#endif
		) {
			ib::error() << "Can't create UNDO tablespace "
				<< name;
		}
		err = DB_ERROR;
	} else if (srv_read_only_mode) {
		ib::info() << name << " opened in read-only mode";
	} else {
		/* We created the data file and now write it full of zeros */

		ib::info() << "Data file " << name << " did not exist: new to"
			" be created";

		ib::info() << "Setting file " << name << " size to "
			<< (SRV_UNDO_TABLESPACE_SIZE_IN_PAGES >> (20 - srv_page_size_shift)) << " MB";

		ib::info() << "Database physically writes the file full: "
			<< "wait...";

		if (!os_file_set_size(name, fh, os_offset_t
				      {SRV_UNDO_TABLESPACE_SIZE_IN_PAGES}
				      << srv_page_size_shift)) {
			ib::error() << "Unable to allocate " << name;
			err = DB_ERROR;
		}

		os_file_close(fh);
	}

	return(err);
}

/* Validate the number of undo opened undo tablespace and user given
undo tablespace
@return DB_SUCCESS if it is valid */
static dberr_t srv_validate_undo_tablespaces()
{
  /* If the user says that there are fewer than what we find we
  tolerate that discrepancy but not the inverse. Because there could
  be unused undo tablespaces for future use. */

  if (srv_undo_tablespaces > srv_undo_tablespaces_open)
  {
    ib::error() << "Expected to open innodb_undo_tablespaces="
                << srv_undo_tablespaces
                << " but was able to find only "
		<< srv_undo_tablespaces_open;

    return DB_ERROR;
  }
  else if (srv_undo_tablespaces_open > 0)
  {
    ib::info() << "Opened " << srv_undo_tablespaces_open
               << " undo tablespaces";

    if (srv_undo_tablespaces == 0)
      ib::warn() << "innodb_undo_tablespaces=0 disables"
                 " dedicated undo log tablespaces";
  }
  return DB_SUCCESS;
}

/** @return the number of active undo tablespaces (except system tablespace) */
static ulint trx_rseg_get_n_undo_tablespaces()
{
  std::set<uint32_t> space_ids;
  mtr_t mtr;
  mtr.start();

  if (const buf_block_t *sys_header= trx_sysf_get(&mtr, false))
    for (ulint rseg_id= 0; rseg_id < TRX_SYS_N_RSEGS; rseg_id++)
      if (trx_sysf_rseg_get_page_no(sys_header, rseg_id) != FIL_NULL)
        if (uint32_t space= trx_sysf_rseg_get_space(sys_header, rseg_id))
          space_ids.insert(space);
  mtr.commit();
  return space_ids.size();
}

/** Open an undo tablespace.
@param[in]	create	whether undo tablespaces are being created
@param[in]	name	tablespace file name
@param[in]	i	undo tablespace count
@return undo tablespace identifier
@retval 0               if file doesn't exist
@retval ULINT_UNDEFINED if page0 is corrupted */
static ulint srv_undo_tablespace_open(bool create, const char* name, ulint i)
{
  bool success;
  ulint space_id= 0;
  ulint fsp_flags= 0;

  if (create)
  {
    space_id= srv_undo_space_id_start + i;
    switch (srv_checksum_algorithm) {
    case SRV_CHECKSUM_ALGORITHM_FULL_CRC32:
    case SRV_CHECKSUM_ALGORITHM_STRICT_FULL_CRC32:
      fsp_flags= FSP_FLAGS_FCRC32_MASK_MARKER | FSP_FLAGS_FCRC32_PAGE_SSIZE();
      break;
    default:
      fsp_flags= FSP_FLAGS_PAGE_SSIZE();
    }
  }

  pfs_os_file_t fh= os_file_create(innodb_data_file_key, name, OS_FILE_OPEN |
                                   OS_FILE_ON_ERROR_NO_EXIT |
                                   OS_FILE_ON_ERROR_SILENT,
                                   OS_DATA_FILE,
                                   srv_read_only_mode, &success);

  if (!success)
    return 0;

  ulint n_retries = 5;
  os_offset_t size= os_file_get_size(fh);
  ut_a(size != os_offset_t(-1));
  page_t *apage= nullptr;
  const page_t *page= nullptr;

  if (!create)
  {
    apage= static_cast<byte*>(aligned_malloc(srv_page_size, srv_page_size));
undo_retry:
    if (os_file_read(IORequestRead, fh, apage, 0, srv_page_size, nullptr) !=
        DB_SUCCESS)
    {
err_exit:
      if (n_retries && srv_operation == SRV_OPERATION_BACKUP)
      {
        sql_print_information("InnoDB: Retrying to read undo "
                              "tablespace %s", name);
        n_retries--;
        goto undo_retry;
      }
      sql_print_error("InnoDB: Unable to read first page of file %s", name);
      aligned_free(apage);
      return ULINT_UNDEFINED;
    }

    page= apage;
    DBUG_EXECUTE_IF("undo_space_read_fail", goto err_exit;);

    uint32_t id= mach_read_from_4(FIL_PAGE_SPACE_ID + page);
    if (id == 0 || id >= SRV_SPACE_ID_UPPER_BOUND ||
        memcmp_aligned<2>(FIL_PAGE_SPACE_ID + page,
                          FSP_HEADER_OFFSET + FSP_SPACE_ID + page, 4))
    {
      sql_print_error("InnoDB: Inconsistent tablespace ID in file %s", name);
      goto err_exit;
    }

    space_id= id;
    fsp_flags= mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);

    if (buf_page_is_corrupted(false, page, fsp_flags))
    {
      page= recv_sys.dblwr.find_page(page_id_t{space_id, 0}, LSN_MAX);
      if (!page)
      {
        /* If the first page of the given user tablespace is not there
        in the doublewrite buffer, then the recovery is going to fail
        now. Report error only when doublewrite buffer is not empty */
        sql_print_error("InnoDB: Corrupted page "
                        "[page id: space=" UINT32PF ", page number=0]"
                        " of datafile '%s' could not be found"
                        " in the doublewrite buffer", space_id, name);
        goto err_exit;
      }

      fsp_flags= mach_read_from_4(FSP_HEADER_OFFSET + FSP_SPACE_FLAGS + page);
    }
  }

  /* Load the tablespace into InnoDB's internal data structures. */

  /* We set the biggest space id to the undo tablespace
  because InnoDB hasn't opened any other tablespace apart
  from the system tablespace. */

  fil_set_max_space_id_if_bigger(space_id);

  mysql_mutex_lock(&fil_system.mutex);
  fil_space_t *space= fil_space_t::create(uint32_t(space_id),
                                          fsp_flags, false, nullptr,
                                          FIL_ENCRYPTION_DEFAULT, true);
  ut_ad(space);
  fil_node_t *file= space->add(name, fh, 0, false, true);

  if (create)
  {
    space->set_sizes(SRV_UNDO_TABLESPACE_SIZE_IN_PAGES);
    space->size= file->size= uint32_t(size >> srv_page_size_shift);
  }
  else if (!file->read_page0(page, true))
  {
    os_file_close(file->handle);
    file->handle= OS_FILE_CLOSED;
    ut_a(fil_system.n_open > 0);
    fil_system.n_open--;
  }

  mysql_mutex_unlock(&fil_system.mutex);
  aligned_free(apage);
  return space_id;
}

/** Check if undo tablespaces and redo log files exist before creating a
new system tablespace
@retval DB_SUCCESS  if all undo and redo logs are not found
@retval DB_ERROR    if any undo and redo logs are found */
static
dberr_t
srv_check_undo_redo_logs_exists()
{
	bool		ret;
	pfs_os_file_t	fh;
	char	name[OS_FILE_MAX_PATH];

	/* Check if any undo tablespaces exist */
	for (ulint i = 1; i <= srv_undo_tablespaces; ++i) {

		snprintf(name, sizeof name, "%s/undo%03zu", srv_undo_dir, i);

		fh = os_file_create(
			innodb_data_file_key, name,
			OS_FILE_OPEN_RETRY
			| OS_FILE_ON_ERROR_NO_EXIT
			| OS_FILE_ON_ERROR_SILENT,
			OS_DATA_FILE,
			srv_read_only_mode,
			&ret);

		if (ret) {
			os_file_close(fh);
			ib::error()
				<< "undo tablespace '" << name << "' exists."
				" Creating system tablespace with existing undo"
				" tablespaces is not supported. Please delete"
				" all undo tablespaces before creating new"
				" system tablespace.";
			return(DB_ERROR);
		}
	}

	/* Check if redo log file exists */
	auto logfilename = get_log_file_path();

	fh = os_file_create(innodb_log_file_key, logfilename.c_str(),
			    OS_FILE_OPEN_RETRY | OS_FILE_ON_ERROR_NO_EXIT
				    | OS_FILE_ON_ERROR_SILENT,
			    OS_LOG_FILE, srv_read_only_mode,
			    &ret);

	if (ret) {
		os_file_close(fh);
		ib::error() << "redo log file '" << logfilename
			    << "' exists. Creating system tablespace with"
			       " existing redo log file is not recommended."
			       " Please delete redo log file before"
			       " creating new system tablespace.";
		return DB_ERROR;
	}

	return(DB_SUCCESS);
}

static dberr_t srv_all_undo_tablespaces_open(bool create_new_db, ulint n_undo)
{
  /* Open all the undo tablespaces that are currently in use. If we
  fail to open any of these it is a fatal error. The tablespace ids
  should be contiguous. It is a fatal error because they are required
  for recovery and are referenced by the UNDO logs (a.k.a RBS). */

  ulint prev_id= create_new_db ? srv_undo_space_id_start - 1 : 0;

  for (ulint i= 0; i < n_undo; ++i)
  {
    char name[OS_FILE_MAX_PATH];
    snprintf(name, sizeof name, "%s/undo%03zu", srv_undo_dir, i + 1);
    ulint space_id= srv_undo_tablespace_open(create_new_db, name, i);
    switch (space_id) {
    case ULINT_UNDEFINED:
      return DB_CORRUPTION;
    case 0:
      if (!create_new_db)
        goto unused_undo;
      sql_print_error("InnoDB: Unable to open create tablespace '%s'", name);
      return DB_ERROR;
    default:
      /* Should be no gaps in undo tablespace ids. */
      ut_a(!i || prev_id + 1 == space_id);
    }

    prev_id= space_id;

    /* Note the first undo tablespace id in case of
    no active undo tablespace. */
    if (0 == srv_undo_tablespaces_open++)
      srv_undo_space_id_start= space_id;
  }

  /* Open any extra unused undo tablespaces. These must be contiguous.
  We stop at the first failure. These are undo tablespaces that are
  not in use and therefore not required by recovery. We only check
  that there are no gaps. */
unused_undo:
  for (ulint i= prev_id + 1; i < srv_undo_space_id_start + TRX_SYS_N_RSEGS;
       ++i)
  {
     char name[OS_FILE_MAX_PATH];
     snprintf(name, sizeof name, "%s/undo%03zu", srv_undo_dir, i);
     ulint space_id= srv_undo_tablespace_open(create_new_db, name, i);
     if (!space_id || space_id == ULINT_UNDEFINED)
       break;
     ++srv_undo_tablespaces_open;
  }

  return srv_validate_undo_tablespaces();
}

/** Open the configured number of dedicated undo tablespaces.
@param[in]	create_new_db	whether the database is being initialized
@return DB_SUCCESS or error code */
dberr_t
srv_undo_tablespaces_init(bool create_new_db)
{
  srv_undo_tablespaces_open= 0;

  ut_a(srv_undo_tablespaces <= TRX_SYS_N_RSEGS);
  ut_a(!create_new_db || srv_operation <= SRV_OPERATION_EXPORT_RESTORED);

  if (srv_undo_tablespaces == 1)
    srv_undo_tablespaces= 0;

  /* Create the undo spaces only if we are creating a new
  instance. We don't allow creating of new undo tablespaces
  in an existing instance (yet). */
  if (create_new_db)
  {
    srv_undo_space_id_start= 1;
    DBUG_EXECUTE_IF("innodb_undo_upgrade", srv_undo_space_id_start= 3;);

    for (ulint i= 0; i < srv_undo_tablespaces; ++i)
    {
      char name[OS_FILE_MAX_PATH];
      snprintf(name, sizeof name, "%s/undo%03zu", srv_undo_dir, i + 1);
      if (dberr_t err= srv_undo_tablespace_create(name))
      {
        ib::error() << "Could not create undo tablespace '" << name << "'.";
        return err;
      }
    }
  }

  /* Get the tablespace ids of all the undo segments excluding
  the system tablespace (0). If we are creating a new instance then
  we build the undo_tablespace_ids ourselves since they don't
  already exist. */
  srv_undo_tablespaces_active= srv_undo_tablespaces;

  ulint n_undo= (create_new_db || srv_operation == SRV_OPERATION_BACKUP ||
                 srv_operation == SRV_OPERATION_RESTORE_DELTA)
    ? srv_undo_tablespaces : TRX_SYS_N_RSEGS;

  mysql_mutex_lock(&recv_sys.mutex);
  dberr_t err= srv_all_undo_tablespaces_open(create_new_db, n_undo);
  mysql_mutex_unlock(&recv_sys.mutex);

  /* Initialize srv_undo_space_id_start=0 when there are no
  dedicated undo tablespaces. */
  if (srv_undo_tablespaces_open == 0)
    srv_undo_space_id_start= 0;

  if (err == DB_SUCCESS && create_new_db)
  {
    mtr_t mtr;
    for (ulint i= 0; i < srv_undo_tablespaces; ++i)
    {
      mtr.start();
      err= fsp_header_init(fil_space_get(srv_undo_space_id_start + i),
                           SRV_UNDO_TABLESPACE_SIZE_IN_PAGES, &mtr);
      mtr.commit();
      if (err)
        break;
    }
  }

  return err;
}

/** Create the temporary file tablespace.
@param[in]	create_new_db	whether we are creating a new database
@return DB_SUCCESS or error code. */
static
dberr_t
srv_open_tmp_tablespace(bool create_new_db)
{
	ulint	sum_of_new_sizes;

	/* Will try to remove if there is existing file left-over by last
	unclean shutdown */
	srv_tmp_space.set_sanity_check_status(true);
	srv_tmp_space.delete_files();
	srv_tmp_space.set_ignore_read_only(true);

	ib::info() << "Creating shared tablespace for temporary tables";

	bool	create_new_temp_space;

	srv_tmp_space.set_space_id(SRV_TMP_SPACE_ID);

	dberr_t	err = srv_tmp_space.check_file_spec(
		&create_new_temp_space, 12 * 1024 * 1024);

	if (err == DB_FAIL) {
		ib::error() << "The innodb_temporary"
			" data file must be writable!";
		err = DB_ERROR;
	} else if (err != DB_SUCCESS) {
		ib::error() << "Could not create the shared innodb_temporary.";
	} else if ((err = srv_tmp_space.open_or_create(
			    true, create_new_db, &sum_of_new_sizes, NULL))
		   != DB_SUCCESS) {
		ib::error() << "Unable to create the shared innodb_temporary";
	} else if (fil_system.temp_space->open(true)) {
		/* Initialize the header page */
		mtr_t mtr;
		mtr.start();
		mtr.set_log_mode(MTR_LOG_NO_REDO);
		err = fsp_header_init(fil_system.temp_space,
				      srv_tmp_space.get_sum_of_sizes(),
				      &mtr);
		mtr.commit();
		if (err == DB_SUCCESS) {
			err = trx_temp_rseg_create(&mtr);
		}
	} else {
		/* This file was just opened in the code above! */
		ib::error() << "The innodb_temporary"
			" data file cannot be re-opened"
			" after check_file_spec() succeeded!";
		err = DB_ERROR;
	}

	return(err);
}

/** Shutdown background threads, except the page cleaner. */
static void srv_shutdown_threads()
{
	ut_ad(!srv_undo_sources);
	srv_master_timer.reset();
	srv_shutdown_state = SRV_SHUTDOWN_EXIT_THREADS;

	if (purge_sys.enabled()) {
		srv_purge_shutdown();
	}

	if (srv_n_fil_crypt_threads) {
		fil_crypt_set_thread_cnt(0);
	}
}


/** Shut down background threads that can generate undo log. */
static void srv_shutdown_bg_undo_sources()
{
  srv_shutdown_state= SRV_SHUTDOWN_INITIATED;

  if (srv_undo_sources)
  {
    ut_ad(!srv_read_only_mode);
    fts_optimize_shutdown();
    dict_stats_shutdown();
    srv_undo_sources= false;
  }
}

#ifdef UNIV_DEBUG
# define srv_init_abort(_db_err)	\
	srv_init_abort_low(create_new_db, __FILE__, __LINE__, _db_err)
#else
# define srv_init_abort(_db_err)	\
	srv_init_abort_low(create_new_db, _db_err)
#endif /* UNIV_DEBUG */

/** Innobase start-up aborted. Perform cleanup actions.
@param[in]	create_new_db	TRUE if new db is  being created
@param[in]	file		File name
@param[in]	line		Line number
@param[in]	err		Reason for aborting InnoDB startup
@return DB_SUCCESS or error code. */
MY_ATTRIBUTE((warn_unused_result, nonnull))
static
dberr_t
srv_init_abort_low(
	bool		create_new_db,
#ifdef UNIV_DEBUG
	const char*	file,
	unsigned	line,
#endif /* UNIV_DEBUG */
	dberr_t		err)
{
	ut_ad(srv_is_being_started);

	if (create_new_db) {
		ib::error() << "Database creation was aborted"
#ifdef UNIV_DEBUG
			" at " << innobase_basename(file) << "[" << line << "]"
#endif /* UNIV_DEBUG */
			" with error " << err << ". You may need"
			" to delete the ibdata1 file before trying to start"
			" up again.";
	} else {
		ib::error() << "Plugin initialization aborted"
#ifdef UNIV_DEBUG
			" at " << innobase_basename(file) << "[" << line << "]"
#endif /* UNIV_DEBUG */
			" with error " << err;
	}

	srv_shutdown_bg_undo_sources();
	srv_shutdown_threads();
	return(err);
}

/** Prepare to delete the redo log file. Flush the dirty pages from all the
buffer pools.  Flush the redo log buffer to the redo log file.
@param[in]	old_exists	old redo log file exists
@return lsn upto which data pages have been flushed. */
static lsn_t srv_prepare_to_delete_redo_log_file(bool old_exists)
{
  DBUG_ENTER("srv_prepare_to_delete_redo_log_file");

  ut_ad(recv_sys.recovery_on);

  /* Clean the buffer pool. */
  buf_flush_sync();

  if (log_sys.log.subformat != 2)
    srv_log_file_size= 0;

  DBUG_EXECUTE_IF("innodb_log_abort_1", DBUG_RETURN(0););
  DBUG_PRINT("ib_log", ("After innodb_log_abort_1"));

  mysql_mutex_lock(&log_sys.mutex);
  const bool latest_format= (log_sys.log.format & ~log_t::FORMAT_ENCRYPTED) ==
    log_t::FORMAT_10_5;
  lsn_t flushed_lsn= log_sys.get_lsn();

  if (latest_format)
  {
    fil_names_clear(flushed_lsn, false);
    flushed_lsn= log_sys.get_lsn();
  }

  {
    const char *msg;
    if (!latest_format || srv_log_file_size == 0)
    {
      msg= "Upgrading redo log: ";
same_size:
      ib::info() << msg << srv_log_file_size_requested << " bytes; LSN="
                 << flushed_lsn;
    }
    else if (old_exists && srv_log_file_size == srv_log_file_size_requested)
    {
      msg= srv_encrypt_log
        ? "Encrypting redo log: " : "Removing redo log encryption: ";
      goto same_size;
    }
    else
    {
      if (srv_encrypt_log == (my_bool)log_sys.is_encrypted())
        msg= srv_encrypt_log ? "Resizing encrypted" : "Resizing";
      else
        msg= srv_encrypt_log
          ? "Encrypting and resizing"
          : "Removing encryption and resizing";

      ib::info() << msg << " redo log from " << srv_log_file_size << " to "
                 << srv_log_file_size_requested
                 << " bytes; LSN=" << flushed_lsn;
    }
  }

  mysql_mutex_unlock(&log_sys.mutex);

  if (flushed_lsn != log_sys.get_flushed_lsn())
  {
    log_write_up_to(flushed_lsn, false);
    log_sys.log.flush();
  }

  ut_ad(flushed_lsn == log_sys.get_lsn());
  ut_ad(!os_aio_pending_reads());
  ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
  ut_ad(!buf_pool.get_oldest_modification(0));
  ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));
  ut_d(os_aio_wait_until_no_pending_writes(false));

  DBUG_RETURN(flushed_lsn);
}

/** Tries to locate LOG_FILE_NAME and check it's size, etc
@param[out]	log_file_found	returns true here if correct file was found
@return	dberr_t with DB_SUCCESS or some error */
static dberr_t find_and_check_log_file(bool &log_file_found)
{
  log_file_found= false;

  auto logfile0= get_log_file_path();
  os_file_stat_t stat_info;
  const dberr_t err= os_file_get_status(logfile0.c_str(), &stat_info, false,
                                        srv_read_only_mode);

  auto is_operation_restore= []() -> bool {
    return srv_operation == SRV_OPERATION_RESTORE ||
           srv_operation == SRV_OPERATION_RESTORE_EXPORT;
  };

  if (err == DB_NOT_FOUND)
  {
    if (is_operation_restore())
      return DB_NOT_FOUND;

    /* This might be first start after mariabackup
    copy-back or move-back. */
    srv_start_after_restore= true;
    return DB_SUCCESS;
  }

  if (stat_info.type != OS_FILE_TYPE_FILE)
    return DB_SUCCESS;

  if (!srv_file_check_mode(logfile0.c_str()))
    return DB_ERROR;

  const os_offset_t size= stat_info.size;
  ut_a(size != (os_offset_t) -1);

  if (size % OS_FILE_LOG_BLOCK_SIZE)
  {
    ib::error() << "Log file " << logfile0 << " size " << size
                << " is not a multiple of " << OS_FILE_LOG_BLOCK_SIZE
                << " bytes";
    return DB_ERROR;
  }

  if (size == 0 && is_operation_restore())
  {
    /* Tolerate an empty LOG_FILE_NAME from a previous run of
    mariabackup --prepare. */
    return DB_NOT_FOUND;
  }
  /* The first log file must consist of at least the following 512-byte pages:
  header, checkpoint page 1, empty, checkpoint page 2, redo log page(s).

  Mariabackup --prepare would create an empty LOG_FILE_NAME. Tolerate it. */
  if (size == 0)
    srv_start_after_restore= true;
  else if (size <= OS_FILE_LOG_BLOCK_SIZE * 4)
  {
    ib::error() << "Log file " << logfile0 << " size " << size
                << " is too small";
    return DB_ERROR;
  }
  srv_log_file_size= size;

  log_file_found= true;
  return DB_SUCCESS;
}

static tpool::task_group rollback_all_recovered_group(1);
static tpool::task rollback_all_recovered_task(trx_rollback_all_recovered,
                                               nullptr,
                                               &rollback_all_recovered_group);

/** Start InnoDB.
@param[in]	create_new_db	whether to create a new database
@return DB_SUCCESS or error code */
dberr_t srv_start(bool create_new_db)
{
	lsn_t		flushed_lsn;
	dberr_t		err		= DB_SUCCESS;
	bool		srv_log_file_found = true;
	mtr_t		mtr;

	ut_ad(srv_operation <= SRV_OPERATION_RESTORE_EXPORT
	      || srv_operation == SRV_OPERATION_RESTORE
	      || srv_operation == SRV_OPERATION_RESTORE_EXPORT);

	if (srv_force_recovery) {
		ib::info() << "!!! innodb_force_recovery is set to "
			<< srv_force_recovery << " !!!";
		if (srv_force_recovery == SRV_FORCE_NO_LOG_REDO) {
			srv_read_only_mode = true;
		}
	}

	if (srv_read_only_mode) {
		sql_print_information("InnoDB: Started in read only mode");
		srv_use_doublewrite_buf = false;
	}

	high_level_read_only = srv_read_only_mode
		|| srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN
		|| srv_sys_space.created_new_raw();

	srv_started_redo = false;

	compile_time_assert(sizeof(ulint) == sizeof(void*));

#ifdef UNIV_DEBUG
	ib::info() << "!!!!!!!! UNIV_DEBUG switched on !!!!!!!!!";
#endif

#ifdef UNIV_IBUF_DEBUG
	ib::info() << "!!!!!!!! UNIV_IBUF_DEBUG switched on !!!!!!!!!";
#endif

	ib::info() << "Compressed tables use zlib " ZLIB_VERSION
#ifdef UNIV_ZIP_DEBUG
	      " with validation"
#endif /* UNIV_ZIP_DEBUG */
	      ;
#ifdef UNIV_ZIP_COPY
	ib::info() << "and extra copying";
#endif /* UNIV_ZIP_COPY */

	/* Since InnoDB does not currently clean up all its internal data
	structures in MySQL Embedded Server Library server_end(), we
	print an error message if someone tries to start up InnoDB a
	second time during the process lifetime. */

	if (srv_start_has_been_called) {
		ib::error() << "Startup called second time"
			" during the process lifetime."
			" In the MariaDB Embedded Server Library"
			" you cannot call server_init() more than"
			" once during the process lifetime.";
	}

	srv_start_has_been_called = true;

	srv_is_being_started = true;

	/* Register performance schema stages before any real work has been
	started which may need to be instrumented. */
	mysql_stage_register("innodb", srv_stages,
			     static_cast<int>(UT_ARR_SIZE(srv_stages)));

	srv_boot();

	ib::info() << my_crc32c_implementation();

	if (!srv_read_only_mode) {
		mysql_mutex_init(srv_monitor_file_mutex_key,
				 &srv_monitor_file_mutex, nullptr);
		mysql_mutex_init(srv_misc_tmpfile_mutex_key,
				 &srv_misc_tmpfile_mutex, nullptr);
	}

	if (!srv_read_only_mode) {
		if (srv_innodb_status) {

			srv_monitor_file_name = static_cast<char*>(
				ut_malloc_nokey(
					strlen(fil_path_to_mysql_datadir)
					+ 20 + sizeof "/innodb_status."));

			sprintf(srv_monitor_file_name,
				"%s/innodb_status." ULINTPF,
				fil_path_to_mysql_datadir,
				static_cast<ulint>
				(IF_WIN(GetCurrentProcessId(), getpid())));

			srv_monitor_file = my_fopen(srv_monitor_file_name,
						    O_RDWR|O_TRUNC|O_CREAT,
						    MYF(MY_WME));

			if (!srv_monitor_file) {
				ib::error() << "Unable to create "
					<< srv_monitor_file_name << ": "
					<< strerror(errno);
				if (err == DB_SUCCESS) {
					err = DB_ERROR;
				}
			}
		} else {

			srv_monitor_file_name = NULL;
			srv_monitor_file = os_file_create_tmpfile();

			if (!srv_monitor_file && err == DB_SUCCESS) {
				err = DB_ERROR;
			}
		}

		srv_misc_tmpfile = os_file_create_tmpfile();

		if (!srv_misc_tmpfile && err == DB_SUCCESS) {
			err = DB_ERROR;
		}
	}

	if (err != DB_SUCCESS) {
		return(srv_init_abort(err));
	}

	if (srv_read_only_mode) {
		ib::info() << "Disabling background log and ibuf IO write"
			<< " threads.";
	}

	if (os_aio_init()) {
		ib::error() << "Cannot initialize AIO sub-system";

		return(srv_init_abort(DB_ERROR));
	}

#ifdef LINUX_NATIVE_AIO
	if (srv_use_native_aio) {
		ib::info() << "Using Linux native AIO";
	}
#endif
#ifdef HAVE_URING
	if (srv_use_native_aio) {
		ib::info() << "Using liburing";
	}
#endif

	fil_system.create(srv_file_per_table ? 50000 : 5000);

	ib::info() << "Initializing buffer pool, total size = "
		<< srv_buf_pool_size
		<< ", chunk size = " << srv_buf_pool_chunk_unit;

	if (buf_pool.create()) {
		ib::error() << "Cannot allocate memory for the buffer pool";

		return(srv_init_abort(DB_ERROR));
	}

	ib::info() << "Completed initialization of buffer pool";

#ifdef UNIV_DEBUG
	/* We have observed deadlocks with a 5MB buffer pool but
	the actual lower limit could very well be a little higher. */

	if (srv_buf_pool_size <= 5 * 1024 * 1024) {

		ib::info() << "Small buffer pool size ("
			<< srv_buf_pool_size / 1024 / 1024
			<< "M), the flst_validate() debug function can cause a"
			<< " deadlock if the buffer pool fills up.";
	}
#endif /* UNIV_DEBUG */

	if (!log_sys.create()) {
		sql_print_error("InnoDB: Cannot allocate memory;"
				" too large innodb_log_buffer_size?");
		return srv_init_abort(DB_ERROR);
	}

	recv_sys.create();
	lock_sys.create(srv_lock_table_size);

	srv_startup_is_before_trx_rollback_phase = true;

	if (!srv_read_only_mode) {
		buf_flush_page_cleaner_init();
		ut_ad(buf_page_cleaner_is_active);
	}

	/* Check if undo tablespaces and redo log files exist before creating
	a new system tablespace */
	srv_log_file_size_requested = srv_log_file_size;

	if (innodb_encrypt_temporary_tables && !log_crypt_init()) {
		return srv_init_abort(DB_ERROR);
	}

	/* Open system tablespace and undo spaces. It is a special case for
	restore with empty redo log file. We support restore in such case and
	proceed after opening system tablespaces. */
	ulint	sum_of_new_sizes;

	auto open_system_spaces = [&]() {
		ut_ad(!create_new_db);
		ut_ad(srv_operation == SRV_OPERATION_RESTORE ||
		      srv_operation == SRV_OPERATION_RESTORE_EXPORT);

		/* Open system tablespace ibdata1. */
		auto err = srv_sys_space.open_or_create(
			false, false, &sum_of_new_sizes, &flushed_lsn);
		if (err != DB_SUCCESS) {
			return srv_init_abort(err);
		}

		mysql_mutex_lock(&recv_sys.mutex);
		bool all_opened = fil_system.sys_space->open(false);
		mysql_mutex_unlock(&recv_sys.mutex);

		/* Open data files for system tablespace. */
		if (!all_opened) {
			err = DB_ERROR;
		} else {
			/* Open undo tablespaces. */
			err = srv_undo_tablespaces_init(false);
		}
		if (err != DB_SUCCESS) {
			return srv_init_abort(err);
		}
		return DB_SUCCESS;
	};

	std::string logfile0;
	bool create_new_log = create_new_db;
	if (create_new_db) {
		err = srv_check_undo_redo_logs_exists();
		if (err != DB_SUCCESS) {
			return(srv_init_abort(DB_ERROR));
		}
		recv_sys.debug_free();
		flushed_lsn = log_sys.get_lsn();
		log_sys.set_flushed_lsn(flushed_lsn);

		err = create_log_file(true, flushed_lsn, logfile0);

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}
	} else {
		srv_log_file_size = 0;

		bool log_file_found;
		if (dberr_t err = find_and_check_log_file(log_file_found)) {
			if (err == DB_NOT_FOUND) {
				/* For restore, we need to continue. */
				return open_system_spaces();
			}
			return srv_init_abort(err);
		}

		create_new_log = srv_log_file_size == 0;
		if (!create_new_log) {
			srv_log_file_found = log_file_found;

			log_sys.log.open_file(get_log_file_path());

			log_sys.log.create();

			if (!log_set_capacity(
				srv_log_file_size_requested)) {
				return(srv_init_abort(DB_ERROR));
			}

			ut_ad(!recv_sys.recovery_on);

			err= recv_recovery_read_max_checkpoint();

			if (err != DB_SUCCESS) {
				return srv_init_abort(err);
			}
		}
	}

	/* Open or create the data files. */
	err = srv_sys_space.open_or_create(
		false, create_new_db, &sum_of_new_sizes, &flushed_lsn);

	switch (err) {
	case DB_SUCCESS:
		break;
	case DB_CANNOT_OPEN_FILE:
		sql_print_error("InnoDB: Could not open or create the system"
			" tablespace. If you tried to add new data files"
			" to the system tablespace, and it failed here,"
			" you should now edit innodb_data_file_path"
			" in my.cnf back to what it was, and remove the"
			" new ibdata files InnoDB created in this failed"
			" attempt. InnoDB only wrote those files full of"
			" zeros, but did not yet use them in any way. But"
			" be careful: do not remove old data files which"
			" contain your precious data!");
		/* fall through */
	default:
		/* Other errors might come from Datafile::validate_first_page() */
		return(srv_init_abort(err));
	}

	if (!create_new_db && create_new_log) {
		if (flushed_lsn < lsn_t(1000)) {
			sql_print_error(
				"InnoDB: Cannot create log file because"
				" data files are corrupt or the"
				" database was not shut down cleanly"
				" after creating the data files.");
			return srv_init_abort(DB_ERROR);
		}

		srv_log_file_size = srv_log_file_size_requested;

		err = create_log_file(false, flushed_lsn, logfile0);
		if (err == DB_SUCCESS) {
			err = create_log_file_rename(flushed_lsn, logfile0);
		}

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		/* Suppress the message about
		crash recovery. */
		flushed_lsn = log_sys.get_lsn();
	}

	/* Open data files in the system tablespace: we keep
	them open until database shutdown */
	mysql_mutex_lock(&recv_sys.mutex);
	ut_d(fil_system.sys_space->recv_size = srv_sys_space_size_debug);
	err = fil_system.sys_space->open(create_new_db)
		? DB_SUCCESS : DB_ERROR;
	mysql_mutex_unlock(&recv_sys.mutex);

	if (err == DB_SUCCESS) {
		err = srv_undo_tablespaces_init(create_new_db);
	}

	/* If the force recovery is set very high then we carry on regardless
	of all errors. Basically this is fingers crossed mode. */

	if (err != DB_SUCCESS
	    && srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {

		return(srv_init_abort(err));
	}

	/* Initialize objects used by dict stats gathering thread, which
	can also be used by recovery if it tries to drop some table */
	if (!srv_read_only_mode) {
		dict_stats_init();
	}

	trx_sys.create();

	if (create_new_db) {
		ut_ad(!srv_read_only_mode);

		mtr_start(&mtr);
		ut_ad(fil_system.sys_space->id == 0);
		compile_time_assert(TRX_SYS_SPACE == 0);
		compile_time_assert(IBUF_SPACE_ID == 0);
		ut_a(fsp_header_init(fil_system.sys_space,
				     uint32_t(sum_of_new_sizes), &mtr)
		     == DB_SUCCESS);

		ulint ibuf_root = btr_create(
			DICT_CLUSTERED | DICT_IBUF, fil_system.sys_space,
			DICT_IBUF_ID_MIN, nullptr, &mtr, &err);

		mtr_commit(&mtr);

		if (ibuf_root == FIL_NULL) {
			return srv_init_abort(err);
		}

		ut_ad(ibuf_root == IBUF_TREE_ROOT_PAGE_NO);

		/* To maintain backward compatibility we create only
		the first rollback segment before the double write buffer.
		All the remaining rollback segments will be created later,
		after the double write buffer has been created. */
		err = trx_sys_create_sys_pages(&mtr);

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		err = dict_create();

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		buf_flush_sync();

		flushed_lsn = log_sys.get_lsn();

		err = fil_write_flushed_lsn(flushed_lsn);

		if (err == DB_SUCCESS) {
			err = create_log_file_rename(flushed_lsn, logfile0);
		}

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}
	} else {
		/* Suppress warnings in fil_space_t::create() for files
		that are being read before dict_boot() has recovered
		DICT_HDR_MAX_SPACE_ID. */
		fil_system.space_id_reuse_warned = true;

		/* We always try to do a recovery, even if the database had
		been shut down normally: this is the normal startup path */

		err = create_new_log
			? DB_SUCCESS
			: recv_recovery_from_checkpoint_start(flushed_lsn);
		recv_sys.close_files();

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		switch (srv_operation) {
                case SRV_OPERATION_NORMAL:
		case SRV_OPERATION_EXPORT_RESTORED:
		case SRV_OPERATION_RESTORE_EXPORT:
			/* Initialize the change buffer. */
			err = dict_boot();
			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}
			/* fall through */
		case SRV_OPERATION_RESTORE:
			/* This must precede recv_sys.apply(true). */
			srv_undo_tablespaces_active
				= trx_rseg_get_n_undo_tablespaces();
			err = srv_validate_undo_tablespaces();
			if (err != DB_SUCCESS) {
				return srv_init_abort(err);
			}
			if (srv_operation != SRV_OPERATION_RESTORE) {
				dict_sys.load_sys_tables();
			}
			err = trx_lists_init_at_db_start();
			if (err != DB_SUCCESS) {
				return srv_init_abort(err);
			}
			break;
		case SRV_OPERATION_RESTORE_DELTA:
		case SRV_OPERATION_BACKUP:
		case SRV_OPERATION_BACKUP_NO_DEFER:
			ut_ad("wrong mariabackup mode" == 0);
		}

		if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO) {
			/* Apply the hashed log records to the
			respective file pages, for the last batch of
			recv_group_scan_log_recs().
			Since it may generate huge batch of threadpool tasks,
			for read io task group, scale down thread creation rate
			by temporarily restricting tpool concurrency.
			*/
			srv_thread_pool->set_concurrency(srv_n_read_io_threads);
			recv_sys.apply(true);
			srv_thread_pool->set_concurrency();

			if (recv_sys.is_corrupt_log()
			    || recv_sys.is_corrupt_fs()) {
				return(srv_init_abort(DB_CORRUPTION));
			}

			DBUG_PRINT("ib_log", ("apply completed"));

			if (recv_needed_recovery) {
				trx_sys_print_mysql_binlog_offset();
			}
		}

		fil_system.space_id_reuse_warned = false;

		if (srv_operation == SRV_OPERATION_RESTORE
		    || srv_operation == SRV_OPERATION_RESTORE_EXPORT) {
			buf_flush_sync();
			recv_sys.debug_free();
			/* After applying the redo log from
			SRV_OPERATION_BACKUP, flush the changes
			to the data files and truncate or delete the log.
			Unless --export is specified, no further change to
			InnoDB files is needed. */
			ut_ad(srv_force_recovery <= SRV_FORCE_IGNORE_CORRUPT);
			ut_ad(recv_no_log_write);
			err = fil_write_flushed_lsn(log_sys.get_lsn());
			ut_ad(!os_aio_pending_reads());
			ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
			ut_ad(!buf_pool.get_oldest_modification(0));
			ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));
			log_sys.log.close_file();
			if (err == DB_SUCCESS) {
				bool trunc = srv_operation
					== SRV_OPERATION_RESTORE;
				if (!trunc) {
					delete_log_file("0");
				} else {
					auto logfile0 = get_log_file_path();
					/* Truncate the first log file. */
					fclose(fopen(logfile0.c_str(), "w"));
				}
			}
			return(err);
		}
		/* Upgrade or resize or rebuild the redo logs before
		generating any dirty pages, so that the old redo log
		file will not be written to. */

		if (srv_force_recovery == SRV_FORCE_NO_LOG_REDO) {
			/* Completely ignore the redo log. */
		} else if (srv_read_only_mode) {
			/* Leave the redo log alone. */
		} else if (srv_log_file_size_requested == srv_log_file_size
			   && srv_log_file_found
			   && log_sys.log.format
			   == (srv_encrypt_log
			       ? log_t::FORMAT_ENC_10_5
			       : log_t::FORMAT_10_5)
			   && log_sys.log.subformat == 2) {
			/* No need to add or remove encryption,
			upgrade, downgrade, or resize. */
		} else {
			/* Prepare to delete the old redo log file */
			flushed_lsn = srv_prepare_to_delete_redo_log_file(
				srv_log_file_found);

			DBUG_EXECUTE_IF("innodb_log_abort_1",
					return(srv_init_abort(DB_ERROR)););
			/* Prohibit redo log writes from any other
			threads until creating a log checkpoint at the
			end of create_log_file(). */
			ut_d(recv_no_log_write = true);
			ut_ad(!os_aio_pending_reads());
			ut_d(mysql_mutex_lock(&buf_pool.flush_list_mutex));
			ut_ad(!buf_pool.get_oldest_modification(0));
			ut_d(mysql_mutex_unlock(&buf_pool.flush_list_mutex));
			/* os_aio_pending_writes() may hold here if
			some write_io_callback() did not release the
			slot yet. However, the page write itself must
			have completed, because the buf_pool.flush_list
			is empty. In debug builds, we wait for this to
			happen, hoping to get a hung process if this
			assumption does not hold. */
			ut_d(os_aio_wait_until_no_pending_writes(false));

			DBUG_EXECUTE_IF("innodb_log_abort_3",
					return(srv_init_abort(DB_ERROR)););
			DBUG_PRINT("ib_log", ("After innodb_log_abort_3"));

			/* Stamp the LSN to the data files. */
			err = fil_write_flushed_lsn(flushed_lsn);

			DBUG_EXECUTE_IF("innodb_log_abort_4", err = DB_ERROR;);
			DBUG_PRINT("ib_log", ("After innodb_log_abort_4"));

			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}

			/* Close the redo log file, so that we can replace it */
			log_sys.log.close_file();

			DBUG_EXECUTE_IF("innodb_log_abort_5",
					return(srv_init_abort(DB_ERROR)););
			DBUG_PRINT("ib_log", ("After innodb_log_abort_5"));

			ib::info()
				<< "Starting to delete and rewrite log file.";

			srv_log_file_size = srv_log_file_size_requested;
			err = create_log_file(false, flushed_lsn, logfile0);

			if (err == DB_SUCCESS) {
				err = create_log_file_rename(flushed_lsn,
							     logfile0);
			}

			if (err != DB_SUCCESS) {
				return(srv_init_abort(err));
			}
		}
		recv_sys.debug_free();

		if (!srv_read_only_mode) {
			const ulint flags = FSP_FLAGS_PAGE_SSIZE();
			for (ulint id = 0; id <= srv_undo_tablespaces; id++) {
				if (fil_space_t* space = fil_space_get(id)) {
					fsp_flags_try_adjust(space, flags);
				}
			}

			if (sum_of_new_sizes > 0) {
				/* New data file(s) were added */
				mtr.start();
				mtr.x_lock_space(fil_system.sys_space);
				buf_block_t* block = buf_page_get(
					page_id_t(0, 0), 0,
					RW_SX_LATCH, &mtr);
				/* The first page of the system tablespace
				should already have been successfully
				accessed earlier during startup. */
				ut_a(block);
				ulint size = mach_read_from_4(
					FSP_HEADER_OFFSET + FSP_SIZE
					+ block->page.frame);
				ut_ad(size == fil_system.sys_space
				      ->size_in_header);
				size += sum_of_new_sizes;
				mtr.write<4>(*block,
					     FSP_HEADER_OFFSET + FSP_SIZE
					     + block->page.frame, size);
				fil_system.sys_space->size_in_header
					= uint32_t(size);
				mtr.commit();
				log_write_up_to(mtr.commit_lsn(), true);
			}
		}

#ifdef UNIV_DEBUG
		{
			mtr.start();
			buf_block_t* block = buf_page_get(page_id_t(0, 0), 0,
							  RW_S_LATCH, &mtr);
			ut_ad(mach_read_from_4(FSP_SIZE + FSP_HEADER_OFFSET
					       + block->page.frame)
			      == fil_system.sys_space->size_in_header);
			mtr.commit();
		}
#endif
		const ulint	tablespace_size_in_header
			= fil_system.sys_space->size_in_header;
		const ulint	sum_of_data_file_sizes
			= srv_sys_space.get_sum_of_sizes();
		/* Compare the system tablespace file size to what is
		stored in FSP_SIZE. In srv_sys_space.open_or_create()
		we already checked that the file sizes match the
		innodb_data_file_path specification. */
		if (srv_read_only_mode
		    || sum_of_data_file_sizes == tablespace_size_in_header) {
			/* Do not complain about the size. */
		} else if (!srv_sys_space.can_auto_extend_last_file()
			   || sum_of_data_file_sizes
			   < tablespace_size_in_header) {
			ib::error() << "Tablespace size stored in header is "
				<< tablespace_size_in_header
				<< " pages, but the sum of data file sizes is "
				<< sum_of_data_file_sizes << " pages";

			if (srv_force_recovery == 0
			    && sum_of_data_file_sizes
			    < tablespace_size_in_header) {
				ib::error() <<
					"Cannot start InnoDB. The tail of"
					" the system tablespace is"
					" missing. Have you edited"
					" innodb_data_file_path in my.cnf"
					" in an inappropriate way, removing"
					" data files from there?"
					" You can set innodb_force_recovery=1"
					" in my.cnf to force"
					" a startup if you are trying to"
					" recover a badly corrupt database.";

				return(srv_init_abort(DB_ERROR));
			}
		}
	}

	ut_ad(err == DB_SUCCESS);
	ut_a(sum_of_new_sizes != ULINT_UNDEFINED);

	/* Create the doublewrite buffer to a new tablespace */
	if (!srv_read_only_mode && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
	    && !buf_dblwr.create()) {
		return(srv_init_abort(DB_ERROR));
	}

	/* Here the double write buffer has already been created and so
	any new rollback segments will be allocated after the double
	write buffer. The default segment should already exist.
	We create the new segments only if it's a new database or
	the database was shutdown cleanly. */

	/* Note: When creating the extra rollback segments during an upgrade
	we violate the latching order, even if the change buffer is empty.
	It cannot create a deadlock because we are still
	running in single threaded mode essentially. Only the IO threads
	should be running at this stage. */

	if (!trx_sys_create_rsegs()) {
		return(srv_init_abort(DB_ERROR));
	}

	if (!create_new_db) {
		ut_ad(high_level_read_only
		      || srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN);

		/* Validate a few system page types that were left
		uninitialized before MySQL or MariaDB 5.5. */
		if (!high_level_read_only
		    && !fil_system.sys_space->full_crc32()) {
			buf_block_t*	block;
			mtr.start();
			/* Bitmap page types will be reset in
			buf_dblwr_check_block() without redo logging. */
			block = buf_page_get(
				page_id_t(IBUF_SPACE_ID,
					  FSP_IBUF_HEADER_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
			corrupted_old_page:
				mtr.commit();
				return srv_init_abort(DB_CORRUPTION);
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			/* Already MySQL 3.23.53 initialized
			FSP_IBUF_TREE_ROOT_PAGE_NO to
			FIL_PAGE_INDEX. No need to reset that one. */
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
				goto corrupted_old_page;
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_TRX_SYS,
					     &mtr);
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE,
					  FSP_FIRST_RSEG_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
				goto corrupted_old_page;
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			block = buf_page_get(
				page_id_t(TRX_SYS_SPACE, FSP_DICT_HDR_PAGE_NO),
				0, RW_X_LATCH, &mtr);
			if (UNIV_UNLIKELY(!block)) {
				goto corrupted_old_page;
			}
			fil_block_check_type(*block, FIL_PAGE_TYPE_SYS, &mtr);
			mtr.commit();
		}

		/* Roll back any recovered data dictionary
		transactions, so that the data dictionary tables will
		be free of any locks.  The data dictionary latch
		should guarantee that there is at most one data
		dictionary transaction active at a time. */
		if (!high_level_read_only
		    && srv_force_recovery <= SRV_FORCE_NO_TRX_UNDO) {
			/* If the following call is ever removed, the
			first-time ha_innobase::open() must hold (or
			acquire and release) a table lock that
			conflicts with trx_resurrect_table_locks(), to
			ensure that any recovered incomplete ALTER
			TABLE will have been rolled back. Otherwise,
			dict_table_t::instant could be cleared by
			rollback invoking
			dict_index_t::clear_instant_alter() while open
			table handles exist in client connections. */
			trx_rollback_recovered(false);
		}

		if (srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {
			/* The following call is necessary for the change
			buffer to work with multiple tablespaces. We must
			know the mapping between space id's and .ibd file
			names.

			We also determine the maximum tablespace id used. */
			dict_check_tablespaces_and_store_max_id(nullptr);
		}

		if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO
		    && !srv_read_only_mode) {
			/* Drop partially created indexes. */
			row_merge_drop_temp_indexes();
			/* Rollback incomplete non-DDL transactions */
			trx_rollback_is_active = true;
			srv_thread_pool->submit_task(&rollback_all_recovered_task);
		}
	}

	srv_startup_is_before_trx_rollback_phase = false;

	if (!srv_read_only_mode) {
		DBUG_EXECUTE_IF("innodb_skip_monitors", goto skip_monitors;);
		/* Create the task which warns of long semaphore waits */
		srv_start_periodic_timer(srv_monitor_timer, srv_monitor_task,
					 SRV_MONITOR_INTERVAL);

#ifndef DBUG_OFF
skip_monitors:
#endif
		ut_ad(srv_force_recovery >= SRV_FORCE_NO_UNDO_LOG_SCAN
		      || !purge_sys.enabled());

		if (srv_force_recovery < SRV_FORCE_NO_BACKGROUND) {
			srv_undo_sources = true;
			/* Create the dict stats gathering task */
			dict_stats_start();
			/* Create the thread that will optimize the
			FULLTEXT search index subsystem. */
			fts_optimize_init();
		}
	}

	err = dict_sys.create_or_check_sys_tables();
	switch (err) {
	case DB_SUCCESS:
		break;
	case DB_READ_ONLY:
		if (srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO) {
			break;
		}
		ib::error() << "Cannot create system tables in read-only mode";
		/* fall through */
	default:
		return(srv_init_abort(err));
	}

	if (!srv_read_only_mode
	    && srv_operation <= SRV_OPERATION_EXPORT_RESTORED) {
		/* Initialize the innodb_temporary tablespace and keep
		it open until shutdown. */
		err = srv_open_tmp_tablespace(create_new_db);

		if (err != DB_SUCCESS) {
			return(srv_init_abort(err));
		}

		if (srv_force_recovery < SRV_FORCE_NO_BACKGROUND) {
			srv_start_periodic_timer(srv_master_timer, srv_master_callback, 1000);
		}
	}

	srv_is_being_started = false;

	if (srv_print_verbose_log) {
		ib::info() << INNODB_VERSION_STR
			   << " started; log sequence number "
			   << recv_sys.recovered_lsn
			   << "; transaction id " << trx_sys.get_max_trx_id();
	}

	if (srv_force_recovery == 0) {
		/* In the change buffer we may have even bigger tablespace
		id's, because we may have dropped those tablespaces, but
		the buffered records have not been cleaned yet. */
		ibuf_update_max_tablespace_id();
	}

	if (!srv_read_only_mode) {
		if (create_new_db) {
			srv_buffer_pool_load_at_startup = FALSE;
		}

#ifdef WITH_WSREP
		/*
		  Create the dump/load thread only when not running with
		  --wsrep-recover.
		*/
		if (!get_wsrep_recovery()) {
#endif /* WITH_WSREP */

		/* Start buffer pool dump/load task */
		buf_load_at_startup();

#ifdef WITH_WSREP
		} else {
			ib::warn() <<
				"Skipping buffer pool dump/restore during "
				"wsrep recovery.";
		}
#endif /* WITH_WSREP */

		/* Create thread(s) that handles key rotation. This is
		needed already here as log_preflush_pool_modified_pages
		will flush dirty pages and that might need e.g.
		fil_crypt_threads_cond. */
		fil_crypt_threads_init();

		/* Initialize online defragmentation. */
		btr_defragment_init();

		srv_started_redo = true;
	}

	return(DB_SUCCESS);
}

/**
  Shutdown purge to make sure that there is no possibility that we call any
  plugin code (e.g., audit) inside virtual column computation.
*/
void innodb_preshutdown()
{
  static bool first_time= true;
  if (!first_time)
    return;
  first_time= false;

  if (srv_read_only_mode)
    return;
  if (!srv_fast_shutdown && srv_operation <= SRV_OPERATION_EXPORT_RESTORED)
  {
    /* Because a slow shutdown must empty the change buffer, we had
    better prevent any further changes from being buffered. */
    innodb_change_buffering= 0;

    if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO && srv_was_started)
      while (trx_sys.any_active_transactions())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  srv_shutdown_bg_undo_sources();
  srv_purge_shutdown();

  if (srv_n_fil_crypt_threads)
    fil_crypt_set_thread_cnt(0);
}


/** Shut down InnoDB. */
void innodb_shutdown()
{
	innodb_preshutdown();
	ut_ad(!srv_undo_sources);
	switch (srv_operation) {
	case SRV_OPERATION_BACKUP:
	case SRV_OPERATION_RESTORE_DELTA:
	case SRV_OPERATION_BACKUP_NO_DEFER:
		break;
	case SRV_OPERATION_RESTORE:
	case SRV_OPERATION_RESTORE_EXPORT:
		mysql_mutex_lock(&buf_pool.flush_list_mutex);
		srv_shutdown_state = SRV_SHUTDOWN_CLEANUP;
		while (buf_page_cleaner_is_active) {
			pthread_cond_signal(&buf_pool.do_flush_list);
			my_cond_wait(&buf_pool.done_flush_list,
				     &buf_pool.flush_list_mutex.m_mutex);
		}
		mysql_mutex_unlock(&buf_pool.flush_list_mutex);
		break;
	case SRV_OPERATION_NORMAL:
	case SRV_OPERATION_EXPORT_RESTORED:
		/* Shut down the persistent files. */
		logs_empty_and_mark_files_at_shutdown();
	}

	os_aio_free();
	fil_space_t::close_all();
	/* Exit any remaining threads. */
	ut_ad(!buf_page_cleaner_is_active);
	srv_shutdown_threads();

	if (srv_monitor_file) {
		my_fclose(srv_monitor_file, MYF(MY_WME));
		srv_monitor_file = 0;
		if (srv_monitor_file_name) {
			unlink(srv_monitor_file_name);
			ut_free(srv_monitor_file_name);
		}
	}

	if (srv_misc_tmpfile) {
		my_fclose(srv_misc_tmpfile, MYF(MY_WME));
		srv_misc_tmpfile = 0;
	}

	ut_ad(dict_sys.is_initialised() || !srv_was_started);
	ut_ad(trx_sys.is_initialised() || !srv_was_started);
	ut_ad(buf_dblwr.is_created() || !srv_was_started
	      || srv_read_only_mode
	      || srv_force_recovery >= SRV_FORCE_NO_TRX_UNDO);
	ut_ad(lock_sys.is_initialised() || !srv_was_started);
	ut_ad(log_sys.is_initialised() || !srv_was_started);
	ut_ad(ibuf.index || !innodb_change_buffering || !srv_was_started
	      || srv_force_recovery >= SRV_FORCE_NO_DDL_UNDO);

	dict_stats_deinit();

	if (srv_started_redo) {
		ut_ad(!srv_read_only_mode);
		/* srv_shutdown_bg_undo_sources() already invoked
		fts_optimize_shutdown(); dict_stats_shutdown(); */

		fil_crypt_threads_cleanup();
		btr_defragment_shutdown();
	}

	/* This must be disabled before closing the buffer pool
	and closing the data dictionary.  */

#ifdef BTR_CUR_HASH_ADAPT
	if (dict_sys.is_initialised()) {
		btr_search_disable();
	}
#endif /* BTR_CUR_HASH_ADAPT */
	ibuf_close();
	log_sys.close();
	purge_sys.close();
	trx_sys.close();
	buf_dblwr.close();
	lock_sys.close();
	trx_pool_close();

	if (!srv_read_only_mode) {
		mysql_mutex_destroy(&srv_monitor_file_mutex);
		mysql_mutex_destroy(&srv_misc_tmpfile_mutex);
	}

	dict_sys.close();
	btr_search_sys_free();
	srv_free();
	fil_system.close();
	pars_lexer_close();
	recv_sys.close();

	ut_ad(buf_pool.is_initialised() || !srv_was_started);
	buf_pool.close();

	srv_sys_space.shutdown();
	if (srv_tmp_space.get_sanity_check_status()) {
		if (fil_system.temp_space) {
			fil_system.temp_space->close();
		}
		srv_tmp_space.delete_files();
	}
	srv_tmp_space.shutdown();

	if (srv_was_started && srv_print_verbose_log) {
		ib::info() << "Shutdown completed; log sequence number "
			   << srv_shutdown_lsn
			   << "; transaction id " << trx_sys.get_max_trx_id();
	}
	srv_thread_pool_end();
	srv_started_redo = false;
	srv_was_started = false;
	srv_start_has_been_called = false;
}

/** Get the meta-data filename from the table name for a
single-table tablespace.
@param[in]	table		table object
@param[out]	filename	filename
@param[in]	max_len		filename max length */
void
srv_get_meta_data_filename(
	dict_table_t*	table,
	char*		filename,
	ulint		max_len)
{
	ulint		len;
	char*		path;

	/* Make sure the data_dir_path is set. */
	dict_get_and_save_data_dir_path(table);

	const char* data_dir_path = DICT_TF_HAS_DATA_DIR(table->flags)
		? table->data_dir_path : nullptr;
	ut_ad(!DICT_TF_HAS_DATA_DIR(table->flags) || data_dir_path);

	path = fil_make_filepath(data_dir_path, table->name, CFG,
				 data_dir_path != nullptr);
	ut_a(path);
	len = strlen(path);
	ut_a(max_len >= len);

	strcpy(filename, path);

	ut_free(path);
}
