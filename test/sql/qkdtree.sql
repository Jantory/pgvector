SET enable_seqscan = off;
SET qkdtree.beam_width = 8;

CREATE TABLE t (id integer, val vector(3));
INSERT INTO t VALUES
    (1, '[0,0,0]'),
    (2, '[1,2,3]'),
    (3, '[1,1,1]'),
    (4, '[4,4,4]'),
    (5, '[3,3,3]'),
    (6, NULL);

CREATE INDEX ON t USING qkdtree (val vector_l2_ops)
    WITH (branch_factor = 2, leaf_size = 2);

SELECT id, val FROM t ORDER BY val <-> '[2,2,2]' LIMIT 5;

-- Incremental insertion and leaf splitting
INSERT INTO t VALUES (7, '[2,2,2]'), (8, '[2,2,3]');
SELECT id, val FROM t ORDER BY val <-> '[2,2,2]' LIMIT 4;

-- Dead tuple removal and aggregate refresh
DELETE FROM t WHERE id IN (3, 7);
VACUUM t;
SELECT id, val FROM t ORDER BY val <-> '[2,2,2]' LIMIT 4;

DROP TABLE t;
RESET qkdtree.beam_width;
RESET enable_seqscan;
