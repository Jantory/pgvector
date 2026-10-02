SET enable_seqscan = off;

-- type and casts

SELECT '[1,-2,32767]'::intvec(3);
SELECT vector_dims('[1,-2,3]'::intvec);
SELECT '[1,2,3]'::intvec <-> '[4,6,3]'::intvec;
SELECT ARRAY[1,-2,3]::intvec;
SELECT '[1,-2,3]'::vector::intvec;
SELECT '[1,-2,3]'::intvec::vector;

SELECT '[1.5]'::intvec;
SELECT '[32768]'::intvec;
SELECT '[1,2]'::intvec(3);

-- L2 index

CREATE TABLE t (id integer, val intvec(3));
INSERT INTO t VALUES
	(1, '[0,0,0]'),
	(2, '[1,2,3]'),
	(3, '[1,1,1]'),
	(4, NULL);
CREATE INDEX ON t USING ivfflat (val intvec_l2_ops) WITH (lists = 1);

INSERT INTO t VALUES (5, '[1,2,4]');

SELECT id, val FROM t ORDER BY val <-> '[3,3,3]';
SELECT COUNT(*) FROM (SELECT * FROM t ORDER BY val <-> (SELECT NULL::intvec)) t2;

DELETE FROM t WHERE id = 3;
VACUUM t;
SELECT id, val FROM t ORDER BY val <-> '[3,3,3]';

DROP TABLE t;

-- dimensions

CREATE TABLE t (val intvec(4000));
CREATE INDEX ON t USING ivfflat (val intvec_l2_ops);
DROP TABLE t;

CREATE TABLE t (val intvec(4001));
CREATE INDEX ON t USING ivfflat (val intvec_l2_ops);
DROP TABLE t;

RESET enable_seqscan;
