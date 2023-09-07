
SET SESSION "sql_seq_scan" = true;
--
-- gh-3075: Check <ALTER TABLE table ADD COLUMN column> statement.
--
CREATE TABLE T1 (A INT PRIMARY KEY);

--
-- COLUMN keyword is optional. Check it here, but omit it below.
--
ALTER TABLE T1 ADD COLUMN B INT;

--
-- A column with the same name already exists.
--
ALTER TABLE T1 ADD B SCALAR;

--
-- Can't add column to a view.
--
CREATE VIEW v AS SELECT * FROM t1;
ALTER TABLE V ADD C INT;

\set language lua
view = box.space._space.index[2]:select('V')[1]:totable()
view_format = view[7]
f = {type = 'string', nullable_action = 'none', name = 'C', is_nullable = true}
table.insert(view_format, f)
view[5] = 3
view[7] = view_format
box.space._space:replace(view)
\set language sql

DROP VIEW v;

--
-- Check PRIMARY KEY constraint works with an added column.
--
CREATE TABLE PK_CHECK (A INT CONSTRAINT PK PRIMARY KEY);
ALTER TABLE pk_check DROP CONSTRAINT pk;
ALTER TABLE PK_CHECK ADD B INT PRIMARY KEY;
INSERT INTO pk_check VALUES (1, 1);
INSERT INTO pk_check VALUES (1, 1);
DROP TABLE pk_check;

--
-- Check UNIQUE constraint works with an added column.
--
CREATE TABLE UNIQUE_CHECK (A INT PRIMARY KEY);
ALTER TABLE UNIQUE_CHECK ADD B INT UNIQUE;
INSERT INTO unique_check VALUES (1, 1);
INSERT INTO unique_check VALUES (2, 1);
DROP TABLE unique_check;

--
-- Check CHECK constraint works with an added column.
--
CREATE TABLE CK_CHECK (A INT PRIMARY KEY);
ALTER TABLE CK_CHECK ADD B INT CHECK (B > 0);
INSERT INTO ck_check VALUES (1, 0);
INSERT INTO ck_check VALUES (1, 1);
DROP TABLE ck_check;
DELETE FROM "_func" WHERE "name" == 'check_CK_CHECK_ck_unnamed_CK_CHECK_B_1';

--
-- Check FOREIGN KEY constraint works with an added column.
--
CREATE TABLE FK_CHECK (A INT PRIMARY KEY);
ALTER TABLE FK_CHECK ADD B INT REFERENCES T1(A);
INSERT INTO fk_check VALUES (0, 1);
INSERT INTO fk_check VALUES (2, 0);
INSERT INTO fk_check VALUES (2, 1);
INSERT INTO t1 VALUES (1, 1);
INSERT INTO fk_check VALUES (2, 1);
DROP TABLE fk_check;
DROP TABLE t1;
--
-- Check FOREIGN KEY (self-referenced) constraint works with an
-- added column.
--
CREATE TABLE SELF (ID INT PRIMARY KEY AUTOINCREMENT, A INT UNIQUE)
ALTER TABLE SELF ADD B INT REFERENCES SELF(A)
INSERT INTO self(a,b) VALUES(1, NULL);
INSERT INTO self(a,b) VALUES(2, 1);
UPDATE self SET b = 2;
UPDATE self SET b = 3;
UPDATE self SET a = 3;
DROP TABLE self;

--
-- Check AUTOINCREMENT works with an added column.
--
CREATE TABLE AUTOINC_CHECK (A INT CONSTRAINT PK PRIMARY KEY);
ALTER TABLE autoinc_check DROP CONSTRAINT pk;
ALTER TABLE AUTOINC_CHECK ADD B INT PRIMARY KEY AUTOINCREMENT;
INSERT INTO autoinc_check(a) VALUES(1);
INSERT INTO autoinc_check(a) VALUES(1);
TRUNCATE TABLE autoinc_check;

--
-- Can't add second column with AUTOINCREMENT.
--
ALTER TABLE AUTOINC_CHECK ADD C INT AUTOINCREMENT;
DROP TABLE autoinc_check;

--
-- Check COLLATE clause works with an added column.
--
CREATE TABLE COLLATE_CHECK (A INT PRIMARY KEY);
ALTER TABLE COLLATE_CHECK ADD B TEXT COLLATE "unicode_ci";
INSERT INTO collate_check VALUES (1, 'a');
INSERT INTO collate_check VALUES (2, 'A');
SELECT * FROM collate_check WHERE b LIKE 'a';
DROP TABLE collate_check;

--
-- Check DEFAULT clause works with an added column.
--
CREATE TABLE DEFAULT_CHECK (A INT PRIMARY KEY);
ALTER TABLE DEFAULT_CHECK ADD B TEXT DEFAULT ('a');
INSERT INTO default_check(a) VALUES (1);
SELECT * FROM default_check;
DROP TABLE default_check;

--
-- Check NULL constraint works with an added column.
--
CREATE TABLE NULL_CHECK (A INT PRIMARY KEY);
ALTER TABLE NULL_CHECK ADD B TEXT NULL;
INSERT INTO null_check(a) VALUES (1);
SELECT * FROM null_check;
DROP TABLE null_check;

--
-- Check NOT NULL constraint works with an added column.
--
CREATE TABLE NOTNULL_CHECK (A INT PRIMARY KEY);
ALTER TABLE NOTNULL_CHECK ADD B TEXT NOT NULL;
INSERT INTO notnull_check(a) VALUES (1);
INSERT INTO notnull_check VALUES (1, 'not null');
DROP TABLE notnull_check;

--
-- Can't add a column with DEAFULT or NULL to a non-empty space.
-- This ability isn't implemented yet.
--
CREATE TABLE NON_EMPTY (A INT PRIMARY KEY);
INSERT INTO non_empty VALUES (1);
ALTER TABLE NON_EMPTY ADD B INT NULL;
ALTER TABLE NON_EMPTY ADD B INT DEFAULT (1);
DROP TABLE non_empty;

--
-- Add to a no-SQL adjusted space without format.
--
\set language lua
_ = box.schema.space.create('WITHOUT_FORMAT')
\set language sql
ALTER TABLE WITHOUT_FORMAT ADD A INT PRIMARY KEY;
INSERT INTO without_format VALUES (1);
DROP TABLE without_format;

--
-- Add to a no-SQL adjusted space with format.
--
\set language lua
with_format = box.schema.space.create('WITH_FORMAT')
with_format:format{{name = 'A', type = 'unsigned'}}
\set language sql
ALTER TABLE WITH_FORMAT ADD B INT PRIMARY KEY;
INSERT INTO with_format VALUES (1, 1);
DROP TABLE with_format;

--
-- Add multiple columns (with a constraint) inside a transaction.
--
CREATE TABLE T2 (A INT PRIMARY KEY)
\set language lua
box.begin()                                                                     \
box.execute('ALTER TABLE T2 ADD B INT')                                         \
box.execute('ALTER TABLE T2 ADD C INT UNIQUE')                                  \
box.commit()
\set language sql
INSERT INTO t2 VALUES (1, 1, 1);
INSERT INTO t2 VALUES (2, 1, 1);
SELECT * FROM t2;
DROP TABLE t2;
