/* ------------------------------------------------------------------------
 *
 * hash.sql
 *		HASH partitioning functions
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

/*
 * Creates hash partitions for specified relation
 */
CREATE OR REPLACE FUNCTION @extschema@.create_hash_partitions(
	parent_relid		REGCLASS,
	attribute			TEXT,
	partitions_count	INT4,
	partition_data		BOOLEAN DEFAULT TRUE,
	partition_names		TEXT[] DEFAULT NULL,
	tablespaces			TEXT[] DEFAULT NULL)
RETURNS INTEGER AS
$$
BEGIN
	PERFORM @extschema@.validate_relname(parent_relid);

	IF partition_data = true THEN
		/* Acquire data modification lock */
		PERFORM @extschema@.prevent_relation_modification(parent_relid);
	ELSE
		/* Acquire lock on parent */
		PERFORM @extschema@.lock_partitioned_relation(parent_relid);
	END IF;

	attribute := lower(attribute);
	PERFORM @extschema@.common_relation_checks(parent_relid, attribute);

	/* Insert new entry to pathman config */
	INSERT INTO @extschema@.pathman_config (partrel, attname, parttype)
	VALUES (parent_relid, attribute, 1);

	/* Create partitions */
	PERFORM @extschema@.create_hash_partitions_internal(parent_relid,
														attribute,
														partitions_count,
														partition_names,
														tablespaces);

	/* Notify backend about changes */
	PERFORM @extschema@.on_create_partitions(parent_relid);

	/* Copy data */
	IF partition_data = true THEN
		PERFORM @extschema@.set_enable_parent(parent_relid, false);
		PERFORM @extschema@.partition_data(parent_relid);
	ELSE
		PERFORM @extschema@.set_enable_parent(parent_relid, true);
	END IF;

	RETURN partitions_count;
END
$$ LANGUAGE plpgsql
SET client_min_messages = WARNING;

/*
 * Replace hash partition with another one. It could be useful in case when
 * someone wants to attach foreign table as a partition.
 *
 * lock_parent - should we take an exclusive lock?
 */
CREATE OR REPLACE FUNCTION @extschema@.replace_hash_partition(
	old_partition		REGCLASS,
	new_partition		REGCLASS,
	lock_parent			BOOL DEFAULT TRUE)
RETURNS REGCLASS AS
$$
DECLARE
	parent_relid		REGCLASS;
	part_attname		TEXT;		/* partitioned column */
	old_constr_name		TEXT;		/* name of old_partition's constraint */
	old_constr_def		TEXT;		/* definition of old_partition's constraint */
	rel_persistence		CHAR;
	p_init_callback		REGPROCEDURE;

BEGIN
	PERFORM @extschema@.validate_relname(old_partition);
	PERFORM @extschema@.validate_relname(new_partition);

	/* Parent relation */
	parent_relid := @extschema@.get_parent_of_partition(old_partition);

	IF lock_parent THEN
		/* Acquire data modification lock (prevent further modifications) */
		PERFORM @extschema@.prevent_relation_modification(parent_relid);
	ELSE
		/* Acquire lock on parent */
		PERFORM @extschema@.lock_partitioned_relation(parent_relid);
	END IF;

	/* Acquire data modification lock (prevent further modifications) */
	PERFORM @extschema@.prevent_relation_modification(old_partition);
	PERFORM @extschema@.prevent_relation_modification(new_partition);

	/* Ignore temporary tables */
	SELECT relpersistence FROM pg_catalog.pg_class
	WHERE oid = new_partition INTO rel_persistence;

	IF rel_persistence = 't'::CHAR THEN
		RAISE EXCEPTION 'temporary table "%" cannot be used as a partition',
						new_partition::TEXT;
	END IF;

	/* Check that new partition has an equal structure as parent does */
	IF NOT @extschema@.tuple_format_is_convertable(parent_relid, new_partition) THEN
		RAISE EXCEPTION 'partition must have a compatible tuple format';
	END IF;

	/* Get partitioning key */
	part_attname := attname FROM @extschema@.pathman_config WHERE partrel = parent_relid;
	IF part_attname IS NULL THEN
		RAISE EXCEPTION 'table "%" is not partitioned', parent_relid::TEXT;
	END IF;

	/* Fetch name of old_partition's HASH constraint */
	old_constr_name = @extschema@.build_check_constraint_name(old_partition::REGCLASS,
															  part_attname);

	/* Fetch definition of old_partition's HASH constraint */
	SELECT pg_catalog.pg_get_constraintdef(oid) FROM pg_catalog.pg_constraint
	WHERE conrelid = old_partition AND conname = old_constr_name
	INTO old_constr_def;

	/* Detach old partition */
	EXECUTE format('ALTER TABLE %s NO INHERIT %s', old_partition, parent_relid);
	EXECUTE format('ALTER TABLE %s DROP CONSTRAINT %s',
				   old_partition,
				   old_constr_name);

	/* Attach the new one */
	EXECUTE format('ALTER TABLE %s INHERIT %s', new_partition, parent_relid);
	EXECUTE format('ALTER TABLE %s ADD CONSTRAINT %s %s',
				   new_partition,
				   @extschema@.build_check_constraint_name(new_partition::REGCLASS,
														   part_attname),
				   old_constr_def);

	/* Fetch init_callback from 'params' table */
	WITH stub_callback(stub) as (values (0))
	SELECT init_callback
	FROM stub_callback
	LEFT JOIN @extschema@.pathman_config_params AS params
	ON params.partrel = parent_relid
	INTO p_init_callback;

	/* Finally invoke init_callback */
	PERFORM @extschema@.invoke_on_partition_created_callback(parent_relid,
															 new_partition,
															 p_init_callback);

	/* Invalidate cache */
	PERFORM @extschema@.on_update_partitions(parent_relid);

	RETURN new_partition;
END
$$
LANGUAGE plpgsql;

/*
 * Just create HASH partitions, called by create_hash_partitions().
 */
CREATE OR REPLACE FUNCTION @extschema@.create_hash_partitions_internal(
	parent_relid		REGCLASS,
	attribute			TEXT,
	partitions_count	INT4,
	partition_names		TEXT[] DEFAULT NULL,
	tablespaces			TEXT[] DEFAULT NULL)
RETURNS VOID AS 'pg_pathman', 'create_hash_partitions_internal'
LANGUAGE C;

/*
 * Returns hash function OID for specified type
 */
CREATE OR REPLACE FUNCTION @extschema@.get_type_hash_func(REGTYPE)
RETURNS REGPROC AS 'pg_pathman', 'get_type_hash_func'
LANGUAGE C STRICT;

/*
 * Calculates hash for integer value
 */
CREATE OR REPLACE FUNCTION @extschema@.get_hash_part_idx(INT4, INT4)
RETURNS INTEGER AS 'pg_pathman', 'get_hash_part_idx'
LANGUAGE C STRICT;

/*
 * Build hash condition for a CHECK CONSTRAINT
 */
CREATE OR REPLACE FUNCTION @extschema@.build_hash_condition(
	attribute_type		REGTYPE,
	attribute			TEXT,
	partitions_count	INT4,
	partition_index		INT4)
RETURNS TEXT AS 'pg_pathman', 'build_hash_condition'
LANGUAGE C STRICT;
