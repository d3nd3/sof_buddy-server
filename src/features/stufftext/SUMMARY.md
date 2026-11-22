# stufftext

Server-console command `stufftext <slot|all|name> <command...>` sends the
stock `svc_stufftext` opcode to one client, every client, or the first
case-insensitive userinfo-name match. It also provides `stufftext_reconnect`,
which forces a clean client reconnect with a welcome line while temporarily
allowing connects during the chain.
