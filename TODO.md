# TO AI Agent

- After you complete a task, give some not under the task

# TODO

- [x] http_order_accepter only do async response. i.e. Response always 200 if recv on server, keep order exec status other way to express.
- [x] since change the way to notice execution report, we use a stdout execution reporter on order_core.
- [x] make a cpp ws client to test L2Publisher
- [x] make app/level_2_publisher.cpp complete:
    - store L2 OrderBook inside level_2_publisher
    - default send nothing to ws client until subscribed
    - handle symbol-wise subscription/unsubscription
    - send a empty frame (Side=None) to imply clear old data, then follows snapshot data, this is the formal behavior for snapshot
- [x] complete ws_client.cpp, can share the same L2 book struct in level_2_publisher
- [x] Currently a new ws client subscribe will not get current snapshot, need to lock current L2 book and send to only the new client
