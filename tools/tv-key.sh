#!/bin/sh
# Send one remote key to the TV input service.
# Run this on the TV (ssh tv 'sh /tmp/tv-key.sh EXIT').
#
# /tmp/netinput.pointer.sock takes a 32-bit big-endian length and then
# "type:button\nname:<KEY>\n\n". Closing the socket immediately drops the
# key. This holds it open for 1.5 s after the write.
key=${1:?usage: tv-key.sh KEYNAME}
exec python - "$key" <<'PY'
import socket, struct, sys, time
key = sys.argv[1]
msg = "type:button\nname:%s\n\n" % key
payload = struct.pack(">I", len(msg)) + msg
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/tmp/netinput.pointer.sock")
s.sendall(payload)
time.sleep(1.5)
s.close()
PY
