/**
 * Confirm that write commands pass through auditing info.
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: {rs0: {nodes: 2}}});

    var admin = st.s.getDB("admin");
    var db = st.s.getDB("audit1");
    var oplog = st.shard0.getDB('local').oplog.rs;

    assert.commandWorked(admin.runCommand({enableSharding: "audit1"}));
    assert.commandWorked(admin.runCommand({shardCollection: "audit1.foo", key: {field: 1}}));

    var assertCommandAudit = function(name, command) {
        command['oplogAudit'] = name;
        printjson(command);
        assert.commandWorked(db.runCommand(command));
        var last = oplog.find({ns: "audit1.foo"}).limit(1).sort({$natural: -1}).next();
        printjson(last);
        assert.eq(name, last.oplogAudit);
    };
    assertCommandAudit("insert", {insert: "foo", documents: [{field: 5}]});
    assertCommandAudit("update", {update: "foo", updates: [{q: {field: 5}, u: {$set: {a: 2}}}]});
    assertCommandAudit("findAndModify", {findAndModify: "foo", query: {field: 5}, update: {$set: {a: 3}}});
    assertCommandAudit("delete", {delete: "foo", deletes: [{q: {field: 5}, limit: 1}]});

    st.stop();
})();
