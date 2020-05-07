-- This file and its contents are licensed under the Apache License 2.0.
-- Please see the included NOTICE for copyright information and
-- LICENSE-APACHE for a copy of the license.

-- Set this variable to avoid using a hard-coded path each time query
-- results are compared
\set QUERY_RESULT_TEST_EQUAL_RELPATH 'include/query_result_test_equal.sql'

-- DROP a table's column before making it a hypertable
CREATE TABLE alter_before(id serial, time timestamp, temp float, colorid integer, notes text, notes_2 text);
ALTER TABLE alter_before DROP COLUMN id;
ALTER TABLE alter_before ALTER COLUMN temp SET (n_distinct = 10);
ALTER TABLE alter_before ALTER COLUMN colorid SET (n_distinct = 11);
ALTER TABLE alter_before ALTER COLUMN colorid RESET (n_distinct);
ALTER TABLE alter_before ALTER COLUMN temp SET STATISTICS 100;
ALTER TABLE alter_before ALTER COLUMN notes SET STORAGE EXTERNAL;

SELECT create_hypertable('alter_before', 'time', chunk_time_interval => 2628000000000);

INSERT INTO alter_before VALUES ('2017-03-22T09:18:22', 23.5, 1);

SELECT * FROM alter_before;

-- Show that deleted column is marked as dropped and that attnums are
-- now different for the root table and the chunk
SELECT c.relname, a.attname, a.attnum, a.attoptions, a.attstattarget, a.attstorage FROM pg_attribute a, pg_class c
WHERE a.attrelid = c.oid
AND (c.relname LIKE '_hyper_1%_chunk' OR c.relname = 'alter_before')
AND a.attnum > 0
ORDER BY c.relname, a.attnum;

-- DROP a table's column after making it a hypertable and having data
CREATE TABLE alter_after(id serial, time timestamp, temp float, colorid integer, notes text, notes_2 text);
SELECT create_hypertable('alter_after', 'time', chunk_time_interval => 2628000000000);

-- Create first chunk
INSERT INTO alter_after (time, temp, colorid) VALUES ('2017-03-22T09:18:22', 23.5, 1);

ALTER TABLE alter_after DROP COLUMN id;
ALTER TABLE alter_after ALTER COLUMN temp SET (n_distinct = 10);
ALTER TABLE alter_after ALTER COLUMN colorid SET (n_distinct = 11);
ALTER TABLE alter_after ALTER COLUMN colorid RESET (n_distinct);
ALTER TABLE alter_after ALTER COLUMN colorid SET STATISTICS 101;
ALTER TABLE alter_after ALTER COLUMN notes_2 SET STORAGE EXTERNAL;

-- Creating new chunks after dropping a column should work just fine
INSERT INTO alter_after VALUES ('2017-03-22T09:18:23', 21.5, 1),
                               ('2017-05-22T09:18:22', 36.2, 2),
                               ('2017-05-22T09:18:23', 15.2, 2);

-- Make sure tuple conversion also works with COPY
\COPY alter_after FROM 'data/alter.tsv' NULL AS '';

-- Data should look OK
SELECT * FROM alter_after;

-- Show that attnums are different for chunks created after DROP
-- column
SELECT c.relname, a.attname, a.attnum FROM pg_attribute a, pg_class c
WHERE a.attrelid = c.oid
AND (c.relname LIKE '_hyper_2%_chunk' OR c.relname = 'alter_after')
AND a.attnum > 0
ORDER BY c.relname, a.attnum;

-- Add an ID column again
ALTER TABLE alter_after ADD COLUMN id serial;

INSERT INTO alter_after (time, temp, colorid) VALUES ('2017-08-22T09:19:14', 12.5, 3);

--test thing that we are allowed to do on chunks
ALTER TABLE  _timescaledb_internal._hyper_2_3_chunk ALTER COLUMN temp RESET (n_distinct);
ALTER TABLE  _timescaledb_internal._hyper_2_4_chunk ALTER COLUMN temp SET (n_distinct = 20);
ALTER TABLE  _timescaledb_internal._hyper_2_4_chunk ALTER COLUMN temp SET STATISTICS 201;
ALTER TABLE  _timescaledb_internal._hyper_2_4_chunk ALTER COLUMN notes SET STORAGE EXTERNAL;

SELECT c.relname, a.attname, a.attnum, a.attoptions, a.attstattarget, a.attstorage FROM pg_attribute a, pg_class c
WHERE a.attrelid = c.oid
AND (c.relname LIKE '_hyper_2%_chunk' OR c.relname = 'alter_after')
AND a.attnum > 0
ORDER BY c.relname, a.attnum;

SELECT * FROM alter_after;

-- test setting reloptions
ALTER TABLE  _timescaledb_internal._hyper_2_3_chunk SET (parallel_workers=2);
ALTER TABLE  _timescaledb_internal._hyper_2_4_chunk SET (parallel_workers=4);
ALTER TABLE  _timescaledb_internal._hyper_2_4_chunk RESET (parallel_workers);

SELECT relname, reloptions FROM pg_class WHERE relname IN ('_hyper_2_3_chunk','_hyper_2_4_chunk');

-- Need superuser to ALTER chunks in _timescaledb_internal schema
\c :TEST_DBNAME :ROLE_SUPERUSER
SELECT * FROM _timescaledb_catalog.chunk WHERE id = 2;

-- Rename chunk
ALTER TABLE _timescaledb_internal._hyper_2_2_chunk RENAME TO new_chunk_name;
SELECT * FROM _timescaledb_catalog.chunk WHERE id = 2;

-- Set schema
ALTER TABLE _timescaledb_internal.new_chunk_name SET SCHEMA public;
SELECT * FROM _timescaledb_catalog.chunk WHERE id = 2;

-- Test that we cannot rename chunk columns
\set ON_ERROR_STOP 0
ALTER TABLE public.new_chunk_name RENAME COLUMN time TO newtime;
\set ON_ERROR_STOP 1

-- Test that we can set tablespace of a hypertable
\c :TEST_DBNAME :ROLE_SUPERUSER
SET client_min_messages = ERROR;
DROP TABLESPACE IF EXISTS tablespace1;
DROP TABLESPACE IF EXISTS tablespace2;
SET client_min_messages = NOTICE;
--test hypertable with tables space
CREATE TABLESPACE tablespace1 OWNER :ROLE_DEFAULT_PERM_USER LOCATION :TEST_TABLESPACE1_PATH;
CREATE TABLESPACE tablespace2 OWNER :ROLE_DEFAULT_PERM_USER LOCATION :TEST_TABLESPACE2_PATH;
\c :TEST_DBNAME :ROLE_DEFAULT_PERM_USER

-- Test that we cannot directly change chunk tablespace
\set ON_ERROR_STOP 0
ALTER TABLE public.new_chunk_name SET TABLESPACE tablespace1;
\set ON_ERROR_STOP 1

-- drop all tables to make checking the tests below easier
DROP TABLE alter_before;
DROP TABLE alter_after;
-- should return 0 rows
SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename LIKE '\_hyper\__\__\_chunk' ORDER BY tablename;

CREATE TABLE hyper_in_space(time bigint, temp float, device int);
SELECT create_hypertable('hyper_in_space', 'time', 'device', 4, chunk_time_interval=>1);

INSERT INTO hyper_in_space(time, temp, device) VALUES (1, 20, 1);
INSERT INTO hyper_in_space(time, temp, device) VALUES (3, 21, 2);
INSERT INTO hyper_in_space(time, temp, device) VALUES (5, 23, 1);

SELECT tablename FROM pg_tables WHERE tablespace = 'tablespace1' ORDER BY tablename;

SET default_tablespace = tablespace1;

-- should be inserted in tablespace1 which is now default
INSERT INTO hyper_in_space(time, temp, device) VALUES (11, 24, 3);
SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename LIKE '\_hyper\__\__\_chunk' ORDER BY tablename;

SET default_tablespace TO DEFAULT;

ALTER TABLE hyper_in_space SET TABLESPACE tablespace1;
SELECT tablename FROM pg_tables WHERE tablespace = 'tablespace1' ORDER BY tablename;

-- should be inserted in an existing chunk in the new tablespace,
-- no new chunks
INSERT INTO hyper_in_space(time, temp, device) VALUES (5, 27, 1);

-- the new chunk should be create in the new tablespace
INSERT INTO hyper_in_space(time, temp, device) VALUES (8, 24, 2);
SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename LIKE '\_hyper\__\__\_chunk' ORDER BY tablename;

-- should not fail (unlike attach_tablespace)
ALTER TABLE hyper_in_space SET TABLESPACE tablespace1;

\set ON_ERROR_STOP 0
-- not an empty tablespace
DROP TABLESPACE tablespace1;
\set ON_ERROR_STOP 1

-- show_chunks and drop_chunks output should be the same
\set QUERY1 'SELECT show_chunks(\'hyper_in_space\', 22)::NAME'
\set QUERY2 'SELECT drop_chunks(\'hyper_in_space\', 22)::NAME'
\set ECHO errors
\ir :QUERY_RESULT_TEST_EQUAL_RELPATH
\set ECHO all
SELECT tablename, tablespace FROM pg_tables WHERE tablespace = 'tablespace1' ORDER BY tablename;

\set ON_ERROR_STOP 0
-- should not be able to drop tablespace if a hypertable depends on it
-- even when there are no chunks
DROP TABLESPACE tablespace1;
\set ON_ERROR_STOP 1

DROP TABLE hyper_in_space;

CREATE TABLE hyper_in_space(time bigint, temp float, device int) TABLESPACE tablespace1;
SELECT create_hypertable('hyper_in_space', 'time', 'device', 4, chunk_time_interval=>1);

INSERT INTO hyper_in_space(time, temp, device) VALUES (1, 20, 1);
INSERT INTO hyper_in_space(time, temp, device) VALUES (3, 21, 2);
INSERT INTO hyper_in_space(time, temp, device) VALUES (5, 23, 1);

SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename ~ '_hyper_\d+_\d+_chunk' ORDER BY tablename;

SELECT attach_tablespace('tablespace2', 'hyper_in_space');

\set ON_ERROR_STOP 0
-- should fail as >1 tablespaces are attached
ALTER TABLE hyper_in_space SET TABLESPACE tablespace1;
\set ON_ERROR_STOP 1

SELECT detach_tablespace('tablespace2', 'hyper_in_space');

SELECT * FROM _timescaledb_catalog.tablespace;

-- make sure when using ALTER TABLE, table spaces are not accumulated
-- as in case of attach_tablespace
-- should have one result
SELECT * FROM _timescaledb_catalog.tablespace;
ALTER TABLE hyper_in_space SET TABLESPACE tablespace2;
-- should have one result
SELECT * FROM _timescaledb_catalog.tablespace;
ALTER TABLE hyper_in_space SET TABLESPACE tablespace1;
-- should have one result, (same as the first in the block)
SELECT * FROM _timescaledb_catalog.tablespace;

SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename ~ '_hyper_\d+_\d+_chunk' ORDER BY tablename;
-- attach tb2 <-> ALTER SET tb1 <-> detach tb1 should work
SELECT detach_tablespace('tablespace1', 'hyper_in_space');
INSERT INTO hyper_in_space(time, temp, device) VALUES (5, 23, 1);
INSERT INTO hyper_in_space(time, temp, device) VALUES (7, 23, 1);

SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename ~ '_hyper_\d+_\d+_chunk' ORDER BY tablename;
SELECT * FROM _timescaledb_catalog.tablespace;

-- tablespace functions should handle the default tablespace just as they do others
SELECT attach_tablespace('pg_default', 'hyper_in_space');
SELECT attach_tablespace('tablespace2', 'hyper_in_space');

SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename ~ '_hyper_\d+_\d+_chunk' ORDER BY tablename;
SELECT * FROM _timescaledb_catalog.tablespace;

INSERT INTO hyper_in_space(time, temp, device) VALUES (12, 22, 1);
INSERT INTO hyper_in_space(time, temp, device) VALUES (13, 23, 3);

SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename ~ '_hyper_\d+_\d+_chunk' ORDER BY tablename;


SELECT detach_tablespace('pg_default', 'hyper_in_space');

ALTER TABLE hyper_in_space SET TABLESPACE pg_default;

SELECT tablename, tablespace FROM pg_tables
WHERE tablename = 'hyper_in_space' OR tablename ~ '_hyper_\d+_\d+_chunk' ORDER BY tablename;

SELECT detach_tablespace('pg_default', 'hyper_in_space');

DROP TABLE hyper_in_space;
DROP TABLESPACE tablespace1;
DROP TABLESPACE tablespace2;

-- Make sure we handle ALTER SCHEMA RENAME for hypertable schemas
\c :TEST_DBNAME :ROLE_SUPERUSER

CREATE SCHEMA IF NOT EXISTS original_name;
CREATE TABLE original_name.my_table (
  date timestamp with time zone NOT NULL,
  quantity double precision
);
SELECT create_hypertable('original_name.my_table','date');

INSERT INTO original_name.my_table (date, quantity) VALUES ('2018-07-04T21:00:00+00:00', 8);

ALTER SCHEMA original_name RENAME TO new_name;

DROP TABLE new_name.my_table;
DROP SCHEMA new_name;

-- Now make sure schema is renamed for multiple hypertables, but not hypertables not in the schema
CREATE SCHEMA IF NOT EXISTS original_name;
CREATE TABLE original_name.my_table (
  date timestamp with time zone NOT NULL,
  quantity double precision
);
CREATE TABLE original_name.my_table2 (
  date timestamp with time zone NOT NULL,
  quantity double precision
);
CREATE TABLE regular_table (
  date timestamp with time zone NOT NULL,
  quantity double precision
);
SELECT create_hypertable('original_name.my_table','date');
SELECT create_hypertable('original_name.my_table2','date');
SELECT create_hypertable('regular_table','date');

INSERT INTO original_name.my_table (date, quantity) VALUES ('2018-07-04T21:00:00+00:00', 8);
INSERT INTO original_name.my_table2 (date, quantity) VALUES ('2018-07-04T21:00:00+00:00', 8);
INSERT INTO regular_table (date, quantity) VALUES ('2018-07-04T21:00:00+00:00', 8);

ALTER SCHEMA original_name RENAME TO new_name;

DROP TABLE new_name.my_table;
DROP TABLE new_name.my_table2;
DROP TABLE regular_table;
DROP SCHEMA new_name;

-- These tables should also drop when we drop the whole schema
CREATE SCHEMA IF NOT EXISTS original_name;
CREATE TABLE original_name.my_table (
  date timestamp with time zone NOT NULL,
  quantity double precision
);
CREATE TABLE original_name.my_table2 (
  date timestamp with time zone NOT NULL,
  quantity double precision
);
SELECT create_hypertable('original_name.my_table','date');
SELECT create_hypertable('original_name.my_table2','date');

INSERT INTO original_name.my_table (date, quantity) VALUES ('2018-07-04T21:00:00+00:00', 8);
INSERT INTO original_name.my_table2 (date, quantity) VALUES ('2018-07-04T21:00:00+00:00', 8);

ALTER SCHEMA original_name RENAME TO new_name;

DROP SCHEMA new_name CASCADE;
\dt new_name.*;

-- Make sure we can't rename internal schemas
\set ON_ERROR_STOP 0
ALTER SCHEMA _timescaledb_internal RENAME TO my_new_schema_name;
ALTER SCHEMA _timescaledb_catalog RENAME TO my_new_schema_name;
ALTER SCHEMA _timescaledb_cache RENAME TO my_new_schema_name;
ALTER SCHEMA _timescaledb_config RENAME TO my_new_schema_name;
\set ON_ERROR_STOP 1

-- Make sure we can rename associated schemas
CREATE TABLE my_table (
  date timestamp with time zone NOT NULL,
  quantity double precision
);

SELECT create_hypertable('my_table','date', associated_schema_name => 'my_associated_schema');
INSERT INTO my_table (date, quantity) VALUES ('2018-07-04T21:00:00+00:00', 8);

ALTER SCHEMA my_associated_schema RENAME TO new_associated_schema;
INSERT INTO my_table (date, quantity) VALUES ('2018-08-10T23:00:00+00:00', 20);
-- Make sure the schema name is changed in both catalog tables
SELECT * from _timescaledb_catalog.hypertable;
SELECT * from _timescaledb_catalog.chunk;

DROP TABLE my_table;
