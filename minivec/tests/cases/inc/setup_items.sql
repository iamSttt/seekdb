# inc/setup_items.sql -- included by 09_directives.sql via --source
# (lives in a subdirectory so `harness.py run tests/cases` does not run it alone)
CREATE TABLE items (id BIGINT PRIMARY KEY, name VARCHAR(16));
INSERT INTO items VALUES (3, 'cherry'), (1, 'apple'), (2, 'banana');
