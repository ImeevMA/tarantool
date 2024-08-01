build_path = os.getenv("BUILDDIR")
package.cpath = build_path..'/test/box/?.so;'..build_path..'/test/box/?.dylib;'..package.cpath

box.cfg{}
box.execute([[CREATE TABLE T(I INT PRIMARY KEY, A INT);]])
box.execute([[INSERT INTO T VALUES(1,1), (2,22), (3,333);]])
box.execute([[INSERT INTO T VALUES(4,1), (5,22), (6,333);]])

one = box.lib.load('one')
get_stmt = one:load("get_stmt")
s = get_stmt("SELECT * FROM SEQSCAN T;")
exec_rv_stmt = one:load("exec_rv_stmt")
exec_rv_stmt(s, 0)

get_rv = one:load("get_rv")
r = get_rv("rv_one")
exec_rv_stmt(s, r)

box.space.T:delete({5})
exec_rv_stmt(s, r)
exec_rv_stmt(s, 0)

box.space.T:truncate()
exec_rv_stmt(s, r)
exec_rv_stmt(s, 0)
