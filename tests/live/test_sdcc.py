#!/usr/bin/env python3
"""Secure DCC (SDCC): DCC SCHAT and DCC SSEND put TLS on the connection.
Against a peer in Python (the role KVIrc and Eggdrop play) and between two
erssi: a chat and a file both ways, the passive variants, TLS 1.3 by
default (a TLS 1.2 peer refused until /SET dcc_tls_min_version 1.2), the
key and certificate made 0600 on first use, the SHA-256 of both
certificates shown, a plain answer to a secure offer never connected,
/DCC SCHAT never accepting a plain offer, a record TLS decrypted but no
socket event announces (a chat line longer than one read), and plain DCC
SEND as before.
Usage: test_sdcc.py <build dir> [<source dir>]"""
import hashlib
import os
import re
import shutil
import socket
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402

WAIT = 30  # a deadline for what comes at once: a loaded machine is slow
LOCAL = 2130706433  # 127.0.0.1 as a DCC address
CTCP = re.compile(r"PRIVMSG \S+ :\x01DCC (\S+) (.*)\x01")


def colons(hexdigest):
    """a SHA-256 the way erssi shows it: AB:CD:..."""
    h = hexdigest.upper()
    return ":".join(h[i:i + 2] for i in range(0, len(h), 2))


def main():
    build = Path(sys.argv[1]).resolve()
    work = Path(tempfile.mkdtemp(prefix="live-sdcc."))
    st = {}

    def log(name="alice"):
        try:
            return st[name].all_log().read_text(errors="replace")
        except OSError:
            return ""

    def log_size(name="alice"):
        return len(log(name))

    def wait_log(text, start=0, name="alice", timeout=WAIT):
        return bool(harness.wait_until(lambda: text in log(name)[start:], timeout))

    def wait_log_re(pattern, start=0, name="alice", timeout=WAIT):
        def found():
            return re.search(pattern, log(name)[start:])
        return harness.wait_until(found, timeout)

    def fresh_bob():
        """a new peer nick for each case: nothing a case leaves behind
        (a DCC to bob) can answer for the next one"""
        st["n"] = st.get("n", 0) + 1
        if "bob" in st:
            st["bob"].close()
        st["bob"] = harness.Client(st["port"], f"bob{st['n']}")
        return st["bob"], st["bob"].nick

    def ctcp(verb):
        """the next DCC <verb> CTCP bob gets: its arguments"""
        line = st["bob"].expect(f"\x01DCC {verb} ")
        m = CTCP.search(line)
        assert m and m.group(1) == verb, line
        return m.group(2).split(" ")

    def make_cert(name):
        d = work / f"cert-{name}"
        d.mkdir(exist_ok=True)
        key, crt = d / "key.pem", d / "cert.pem"
        r = subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                            "ec_paramgen_curve:prime256v1", "-nodes", "-subj", f"/CN={name}",
                            "-days", "2", "-keyout", str(key), "-out", str(crt)],
                           capture_output=True, text=True)
        assert r.returncode == 0, r.stderr
        der = ssl.PEM_cert_to_DER_cert(crt.read_text())
        return str(crt), str(key), colons(hashlib.sha256(der).hexdigest())

    def server_ctx(maxver=None):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(st["bob_crt"], st["bob_key"])
        if maxver is not None:
            ctx.maximum_version = maxver
        return ctx

    def client_ctx():
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        ctx.load_cert_chain(st["bob_crt"], st["bob_key"])
        return ctx

    class Peer(threading.Thread):
        """bob's end of one DCC connection, in a thread: listen (as the
        TLS server, or plain) or connect (as the TLS client)"""

        def __init__(self, job, listen=True, ctx=None):
            super().__init__(daemon=True)
            self.job, self.ctx, self.error, self.conn = job, ctx, None, None
            self.version = self.peer_der = None
            if listen:
                self.lsock = socket.socket()
                self.lsock.bind(("127.0.0.1", 0))
                self.lsock.listen(1)
                self.lsock.settimeout(WAIT)
                self.port = self.lsock.getsockname()[1]
            else:
                self.lsock = None

        def connect_to(self, port):
            self.target = port
            self.start()

        def run(self):
            try:
                if self.lsock is not None:
                    sock, _ = self.lsock.accept()
                    self.lsock.close()
                    sock.settimeout(WAIT)
                    if self.ctx is not None:
                        sock = self.ctx.wrap_socket(sock, server_side=True)
                else:
                    sock = socket.create_connection(("127.0.0.1", self.target), WAIT)
                    sock = self.ctx.wrap_socket(sock, server_side=False)
                self.conn = sock
                if isinstance(sock, ssl.SSLSocket):
                    self.version = sock.version()
                    self.peer_der = sock.getpeercert(binary_form=True)
                self.job(sock)
            except Exception as exc:  # noqa: BLE001 - reported by the test
                self.error = exc

        def finish(self):
            self.join(WAIT)
            assert not self.is_alive(), "the peer did not finish"
            if self.error is not None:
                raise AssertionError(f"peer: {self.error!r}")

    def own_fingerprint(name="alice"):
        pem = (st[name].home / "dcc-tls.pem").read_text()
        cert = pem[pem.index("-----BEGIN CERTIFICATE-----"):]
        return colons(hashlib.sha256(ssl.PEM_cert_to_DER_cert(cert)).hexdigest())

    def read_until(sock, text):
        buf = b""
        while text not in buf:
            data = sock.recv(65536)
            if not data:
                break
            buf += data
        return buf

    def t_install():
        if shutil.which("openssl") is None:
            raise harness.Skip("no openssl program for the peer's certificate")
        st["inst"] = harness.install(build, work)

    def t_start():
        ircd = harness.Ircd(work)
        st["ircd"] = ircd
        port = ircd.start()
        st["port"] = port
        st["bob_crt"], st["bob_key"], st["bob_fp"] = make_cert("bob")
        extra = '"irc/dcc" = { dcc_download_path = "@HOME@/dl"; dcc_timeout = "30s"; };'
        for name in ("alice", "carol"):
            text = harness.config(port, name).replace("settings = {", "settings = {\n  " + extra, 1)
            e = harness.Erssi(st["inst"], work, name, text)
            st[name] = e
            (e.home / "dl").mkdir(parents=True, exist_ok=True)
            e.start()
            e.wait_ready()
        for name in ("alice", "carol"):
            assert wait_log("Join to #test was synced", name=name), log(name)
        assert not (st["alice"].home / "dcc-tls.pem").exists()

    def t_schat_peer_listens():
        """bob offers DCC SCHAT, alice accepts it with /DCC CHAT: TLS 1.3,
        lines both ways, alice's key made 0600, both SHA-256 shown"""
        a = st["alice"]
        bob, nick = fresh_bob()
        got = {}

        def job(sock):
            got["line"] = read_until(sock, b"\n")
            sock.sendall(b"hi from bob\n")
            # a line longer than one read of erssi (2048 bytes) in one TLS
            # record: the rest sits decrypted in TLS, no socket event
            sock.sendall(b"L" * 3000 + b" long-line-end\n")
            got["after"] = read_until(sock, b"bye\n")
        peer = Peer(job, ctx=server_ctx())
        peer.start()
        off = log_size()
        bob.send(f"PRIVMSG alice :\x01DCC SCHAT chat {LOCAL} {peer.port}\x01")
        assert wait_log("a secure DCC (SCHAT)", off), log()[off:]
        a.type_and_wait(f"/dcc chat {nick}")
        assert wait_log("encrypted: TLSv1.3", off), log()[off:]
        assert wait_log(f"of {nick}: {st['bob_fp']}", off), log()[off:]
        pem = a.home / "dcc-tls.pem"
        assert stat.S_IMODE(pem.stat().st_mode) == 0o600, oct(pem.stat().st_mode)
        assert wait_log(f"yours: {own_fingerprint()}", off), log()[off:]
        a.type_and_wait(f"/msg ={nick} hello over tls")
        assert wait_log("hi from bob", off), log()[off:]
        assert wait_log("long-line-end", off, timeout=10), "a decrypted record was left in TLS"
        a.type_and_wait(f"/msg ={nick} bye")
        peer.finish()
        assert peer.version == "TLSv1.3", peer.version
        assert got["line"] == b"hello over tls\n", got
        assert got["after"].endswith(b"bye\n"), got
        a.type_and_wait(f"/dcc close chat {nick}")

    def t_schat_alice_listens():
        """alice offers with /DCC SCHAT: bob connects as the TLS client and
        sees the certificate alice says is hers"""
        a = st["alice"]
        _, nick = fresh_bob()
        off = log_size()
        a.type_and_wait(f"/dcc schat {nick}")
        args = ctcp("SCHAT")
        assert args[0].upper() == "CHAT" and args[1] == str(LOCAL), args
        got = {}

        def job(sock):
            sock.sendall(b"hello alice\n")
            got["line"] = read_until(sock, b"\n")
        peer = Peer(job, listen=False, ctx=client_ctx())
        peer.connect_to(int(args[2]))
        assert wait_log("hello alice", off), log()[off:]
        a.type_and_wait(f"/msg ={nick} and back")
        peer.finish()
        assert peer.version == "TLSv1.3", peer.version
        assert colons(hashlib.sha256(peer.peer_der).hexdigest()) == own_fingerprint()
        assert wait_log(f"of {nick}: {st['bob_fp']}", off), log()[off:]
        assert got["line"] == b"and back\n", got
        a.type_and_wait(f"/dcc close chat {nick}")

    def t_tls12_refused():
        """a peer that has only TLS 1.2: refused, then accepted with
        /SET dcc_tls_min_version 1.2"""
        a = st["alice"]
        bob, nick = fresh_bob()
        peer = Peer(lambda sock: None, ctx=server_ctx(ssl.TLSVersion.TLSv1_2))
        peer.start()
        off = log_size()
        bob.send(f"PRIVMSG alice :\x01DCC SCHAT chat {LOCAL} {peer.port}\x01")
        assert wait_log("a secure DCC (SCHAT)", off), log()[off:]
        a.type_and_wait(f"/dcc chat {nick}")
        assert wait_log("TLS failed", off), log()[off:]
        peer.join(WAIT)
        assert peer.error is not None, "the TLS 1.2 handshake succeeded"
        assert "encrypted:" not in log()[off:], log()[off:]

        a.type_and_wait("/set dcc_tls_min_version 1.2")
        got = {}
        peer = Peer(lambda sock: got.setdefault("line", read_until(sock, b"\n")),
                    ctx=server_ctx(ssl.TLSVersion.TLSv1_2))
        peer.start()
        off = log_size()
        bob.send(f"PRIVMSG alice :\x01DCC SCHAT chat {LOCAL} {peer.port}\x01")
        assert wait_log("a secure DCC (SCHAT)", off), log()[off:]
        a.type_and_wait(f"/dcc chat {nick}")
        assert wait_log("encrypted: TLSv1.2", off), log()[off:]
        a.type_and_wait(f"/msg ={nick} twelve")
        peer.finish()
        assert peer.version == "TLSv1.2" and got["line"] == b"twelve\n", (peer.version, got)
        a.type_and_wait(f"/dcc close chat {nick}", "/set dcc_tls_min_version 1.3")

    def t_no_downgrade():
        """a plain answer to alice's passive SCHAT never connects;
        /DCC SCHAT does not accept a plain DCC CHAT offer"""
        a = st["alice"]
        bob, nick = fresh_bob()
        plain = socket.socket()
        plain.bind(("127.0.0.1", 0))
        plain.listen(1)
        plain.settimeout(0.1)
        port = plain.getsockname()[1]
        try:
            off = log_size()
            a.type_and_wait(f"/dcc schat -passive {nick}")
            args = ctcp("SCHAT")
            assert args[1] == "16843009" and args[2] == "0", args
            bob.send(f"PRIVMSG alice :\x01DCC CHAT chat {LOCAL} {port} {args[3]}\x01")
            bob.send(f"PRIVMSG alice :\x01DCC CHAT chat {LOCAL} {port}\x01")
            assert wait_log(f"DCC CHAT from {nick}", off), log()[off:]
            a.type_and_wait(f"/dcc schat {nick}")
            assert wait_log("offered a plain DCC CHAT, not SCHAT", off), log()[off:]
            a.sync_output()
            time.sleep(1)
            try:
                plain.accept()
                raise AssertionError("alice connected to a plain DCC")
            except socket.timeout:
                pass
            assert "established" not in log()[off:], log()[off:]
            a.type_and_wait(f"/dcc close chat {nick}")
        finally:
            plain.close()

    def t_ssend_to_peer():
        """/DCC SSEND: bob gets the file over TLS and acknowledges it"""
        a = st["alice"]
        _, nick = fresh_bob()
        data = os.urandom(300 * 1024 + 7)
        src = a.home_dir / "pic.bin"
        src.write_bytes(data)
        off = log_size()
        a.type_and_wait(f"/dcc ssend {nick} {src}")
        args = ctcp("SSEND")
        assert args[0] == "pic.bin" and args[1] == str(LOCAL) and args[3] == str(len(data)), args
        got = bytearray()

        def job(sock):
            while len(got) < len(data):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                got.extend(chunk)
                sock.sendall(struct.pack("!I", len(got) & 0xFFFFFFFF))
            sock.settimeout(WAIT)
            sock.recv(1)  # alice closes once every byte is acknowledged
        peer = Peer(job, listen=False, ctx=client_ctx())
        peer.connect_to(int(args[2]))
        assert wait_log("DCC sent file pic.bin", off), log()[off:]
        peer.finish()
        assert peer.version == "TLSv1.3", peer.version
        assert bytes(got) == data, (len(got), len(data))
        assert wait_log(f"DCC SSEND with {nick} encrypted: TLSv1.3", off), log()[off:]

    def t_ssend_from_peer():
        """bob offers DCC SSEND, alice takes it with /DCC GET"""
        a = st["alice"]
        bob, nick = fresh_bob()
        data = os.urandom(200 * 1024 + 3)
        acks = []

        def job(sock):
            sock.sendall(data)
            want = struct.pack("!I", len(data))
            buf = b""
            while not buf.endswith(want):
                chunk = sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
            acks.append(buf)
        peer = Peer(job, ctx=server_ctx())
        peer.start()
        off = log_size()
        bob.send(f"PRIVMSG alice :\x01DCC SSEND gift.bin {LOCAL} {peer.port} {len(data)}\x01")
        assert wait_log("a secure DCC (SSEND)", off), log()[off:]
        a.type_and_wait(f"/dcc get {nick}")
        peer.finish()
        peer.conn.close()
        assert wait_log("DCC received file", off), log()[off:]
        assert (a.home / "dl" / "gift.bin").read_bytes() == data
        assert wait_log(f"DCC SGET with {nick} encrypted: TLSv1.3", off), log()[off:]

    def t_passive_ssend():
        """/DCC SSEND -passive: bob listens, answers with SSEND, alice
        connects as the TLS client"""
        a = st["alice"]
        bob, nick = fresh_bob()
        data = os.urandom(64 * 1024)
        src = a.home_dir / "pasv.bin"
        src.write_bytes(data)
        off = log_size()
        a.type_and_wait(f"/dcc ssend -passive {nick} {src}")
        args = ctcp("SSEND")
        assert args[1:3] == ["16843009", "0"] and len(args) == 5, args
        got = bytearray()

        def job(sock):
            while len(got) < len(data):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                got.extend(chunk)
                sock.sendall(struct.pack("!I", len(got)))
            sock.recv(1)
        peer = Peer(job, ctx=server_ctx())
        peer.start()
        bob.send(f"PRIVMSG alice :\x01DCC SSEND pasv.bin {LOCAL} {peer.port} {len(data)} {args[4]}\x01")
        assert wait_log("DCC sent file pasv.bin", off), log()[off:]
        peer.finish()
        assert bytes(got) == data and peer.version == "TLSv1.3"

    def t_plain_send():
        """plain /DCC SEND as before: no TLS, 512-byte writes"""
        a = st["alice"]
        _, nick = fresh_bob()
        data = os.urandom(20 * 1024 + 1)
        src = a.home_dir / "plain.bin"
        src.write_bytes(data)
        off = log_size()
        a.type_and_wait(f"/dcc send {nick} {src}")
        args = ctcp("SEND")
        got = bytearray()
        sock = socket.create_connection(("127.0.0.1", int(args[2])), WAIT)
        try:
            while len(got) < len(data):
                chunk = sock.recv(65536)
                if not chunk:
                    break
                got.extend(chunk)
                sock.sendall(struct.pack("!I", len(got)))
            assert wait_log("DCC sent file plain.bin", off), log()[off:]
        finally:
            sock.close()
        assert bytes(got) == data
        assert "encrypted:" not in log()[off:], log()[off:]

    def t_erssi_to_erssi():
        """alice and carol: SSEND and SCHAT between two erssi, each shows
        the other's certificate as the one the other calls its own"""
        a, c = st["alice"], st["carol"]
        data = os.urandom(512 * 1024 + 11)
        src = a.home_dir / "both.bin"
        src.write_bytes(data)
        offa, offc = log_size(), log_size("carol")
        a.type_and_wait(f"/dcc ssend carol {src}")
        assert wait_log("a secure DCC (SSEND)", offc, name="carol"), log("carol")[offc:]
        c.type_and_wait("/dcc get alice")
        assert wait_log("DCC received file", offc, name="carol"), log("carol")[offc:]
        assert (c.home / "dl" / "both.bin").read_bytes() == data
        assert wait_log("DCC sent file both.bin", offa), log()[offa:]
        fa, fc = own_fingerprint("alice"), own_fingerprint("carol")
        assert fa != fc
        assert wait_log(f"of carol: {fc}, yours: {fa}", offa), log()[offa:]
        assert wait_log(f"of alice: {fa}, yours: {fc}", offc, name="carol"), log("carol")[offc:]

        offa, offc = log_size(), log_size("carol")
        c.type_and_wait("/dcc schat alice")
        assert wait_log("a secure DCC (SCHAT)", offa), log()[offa:]
        a.type_and_wait("/dcc chat carol")
        assert wait_log("encrypted: TLSv1.3", offa), log()[offa:]
        assert wait_log("encrypted: TLSv1.3", offc, name="carol"), log("carol")[offc:]
        a.type_and_wait("/msg =carol secret words")
        assert wait_log("secret words", offc, name="carol"), log("carol")[offc:]
        c.type_and_wait("/dcc list")
        assert wait_log_re(r"alice SCHAT", offc, name="carol"), log("carol")[offc:]
        a.type_and_wait("/dcc close chat carol")

    def combined_pem(name, order, mode=0o600):
        """a single PEM with a key and a matching certificate the two ways
        round (OpenSSL writes them key-then-cert; a real one may be either),
        and its SHA-256; written under <work> with the given mode"""
        crt, key, fp = make_cert(name)
        crt_text, key_text = Path(crt).read_text(), Path(key).read_text()
        body = crt_text + key_text if order == "cert-key" else key_text + crt_text
        p = work / f"custom-{name}-{order}.pem"
        p.write_text(body)
        os.chmod(p, mode)
        return p, fp

    def schat_to_peer(nick):
        """alice offers SCHAT and listens as the TLS server; the peer
        connects as the TLS client, so (client side) it sees the very
        certificate alice presents on this connection. The handshake is all
        these cases need, so the peer just finishes once it is done. Returns
        the Peer (peer.peer_der is alice's certificate) and the log offset."""
        off = log_size()
        st["alice"].type_and_wait(f"/dcc schat {nick}")
        args = ctcp("SCHAT")
        assert args[0].upper() == "CHAT" and args[1] == str(LOCAL), args
        peer = Peer(lambda sock: None, listen=False, ctx=client_ctx())
        peer.connect_to(int(args[2]))
        return peer, off

    def t_custom_cert_both_orders():
        """a valid user certificate (dcc_tls_cert) is used whatever the
        order of its key and certificate, is shown as "yours", and is left
        byte-for-byte unchanged - it is never regenerated the way the
        managed one is (the 2026.11 data-loss bug deleted it)"""
        a = st["alice"]
        for order in ("cert-key", "key-cert"):
            path, fp = combined_pem(f"custom-{order}", order)
            before = path.read_bytes()
            a.type_and_wait(f"/set dcc_tls_cert {path}")
            _, nick = fresh_bob()
            peer, off = schat_to_peer(nick)
            assert wait_log("encrypted: TLSv1.3", off), f"{order}:\n" + log()[off:]
            assert wait_log(f"yours: {fp}", off), f"{order}:\n" + log()[off:]
            peer.finish()
            assert colons(hashlib.sha256(peer.peer_der).hexdigest()) == fp, order
            assert path.read_bytes() == before, f"the user's certificate was changed ({order})"
            a.type_and_wait(f"/dcc close chat {nick}", "/set -clear dcc_tls_cert")

    def refused_schat(nick, expect):
        """alice offers SCHAT and listens; a plain TCP connection to her
        port makes her build the certificate (as the TLS server) - which is
        where a bad user certificate is refused. Returns the log offset and
        asserts EXPECT is logged; the plain socket never does TLS."""
        off = log_size()
        st["alice"].type_and_wait(f"/dcc schat {nick}")
        args = ctcp("SCHAT")
        s = socket.create_connection(("127.0.0.1", int(args[2])), WAIT)
        try:
            assert wait_log(expect, off), log()[off:]
        finally:
            s.close()
            st["alice"].type_and_wait(f"/dcc close chat {nick}")
        return off

    def t_custom_cert_broken_kept():
        """a broken user certificate is refused with a message and left
        exactly as it is: not deleted, not replaced by a generated one"""
        a = st["alice"]
        path = work / "custom-broken.pem"
        path.write_text("-----BEGIN CERTIFICATE-----\nnot a certificate\n-----END CERTIFICATE-----\n")
        os.chmod(path, 0o600)
        before = path.read_bytes()
        a.type_and_wait(f"/set dcc_tls_cert {path}")
        _, nick = fresh_bob()
        refused_schat(nick, "not a usable key and certificate")
        assert path.read_bytes() == before, "the broken user certificate was changed"
        a.type_and_wait("/set -clear dcc_tls_cert")

    def t_custom_cert_world_readable_refused():
        """a user certificate others can read is refused, not silently
        chmod'd or replaced"""
        a = st["alice"]
        path, _ = combined_pem("custom-644", "key-cert", mode=0o644)
        before = path.read_bytes()
        a.type_and_wait(f"/set dcc_tls_cert {path}")
        _, nick = fresh_bob()
        refused_schat(nick, "readable by other users")
        assert stat.S_IMODE(path.stat().st_mode) == 0o644, "the mode was changed"
        assert path.read_bytes() == before, "the user certificate was changed"
        a.type_and_wait("/set -clear dcc_tls_cert")

    def t_backup_not_deleted():
        """an unrelated neighbour of the managed certificate, such as
        dcc-tls.pem.backup, is not swept as a stale temporary"""
        a = st["alice"]
        backup = a.home / "dcc-tls.pem.backup"
        backup.write_text("not ours\n")
        os.utime(backup, (time.time() - 600, time.time() - 600))
        _, nick = fresh_bob()
        peer, off = schat_to_peer(nick)
        assert wait_log("encrypted: TLSv1.3", off), log()[off:]
        peer.finish()
        assert backup.exists(), "dcc-tls.pem.backup was deleted"
        a.type_and_wait(f"/dcc close chat {nick}")

    def t_managed_world_readable_renewed():
        """the managed key loosened to 0644 is renewed (a leaked key is not
        made safe by a chmod) and the connection still works"""
        a = st["alice"]
        pem = a.home / "dcc-tls.pem"
        assert pem.exists(), "the managed certificate should exist by now"
        before_fp = own_fingerprint()
        os.chmod(pem, 0o644)
        _, nick = fresh_bob()
        peer, off = schat_to_peer(nick)
        assert wait_log("encrypted: TLSv1.3", off), log()[off:]
        peer.finish()
        assert stat.S_IMODE(pem.stat().st_mode) == 0o600, oct(pem.stat().st_mode)
        assert own_fingerprint() != before_fp, "the world-readable managed key was kept"
        a.type_and_wait(f"/dcc close chat {nick}")

    def t_fifo_does_not_hang():
        """a FIFO in the managed certificate's place does not hang the main
        loop: erssi keeps answering and replaces it with a real key"""
        a = st["alice"]
        pem = a.home / "dcc-tls.pem"
        if pem.exists() or pem.is_symlink():
            pem.unlink()
        os.mkfifo(pem)
        _, nick = fresh_bob()
        peer = Peer(lambda sock: None, ctx=server_ctx())
        peer.start()
        off = log_size()
        st["bob"].send(f"PRIVMSG alice :\x01DCC SCHAT chat {LOCAL} {peer.port}\x01")
        assert wait_log("a secure DCC (SCHAT)", off), log()[off:]
        a.type_and_wait(f"/dcc chat {nick}")
        a.sync_output()  # fails if the event loop is stuck in open()
        assert wait_log("encrypted: TLSv1.3", off), log()[off:]
        peer.finish()
        assert stat.S_ISREG(pem.stat().st_mode), "the FIFO was not replaced"
        a.type_and_wait(f"/dcc close chat {nick}")

    def t_fingerprint_follows_live_cert():
        """"yours" is the certificate on this connection, not a cached one:
        after the managed PEM is regenerated the next connection shows the
        new fingerprint, matching what the peer sees"""
        a = st["alice"]
        _, nick = fresh_bob()
        peer1, off = schat_to_peer(nick)
        assert wait_log("encrypted: TLSv1.3", off), log()[off:]
        peer1.finish()
        fp1 = colons(hashlib.sha256(peer1.peer_der).hexdigest())
        assert wait_log(f"yours: {fp1}", off), log()[off:]
        a.type_and_wait(f"/dcc close chat {nick}")
        # force a fresh managed certificate under the same path
        (a.home / "dcc-tls.pem").write_text("broken\n")
        os.chmod(a.home / "dcc-tls.pem", 0o600)
        _, nick = fresh_bob()
        peer2, off = schat_to_peer(nick)
        assert wait_log("encrypted: TLSv1.3", off), log()[off:]
        peer2.finish()
        fp2 = colons(hashlib.sha256(peer2.peer_der).hexdigest())
        assert fp2 != fp1, "the certificate was not regenerated"
        assert wait_log(f"yours: {fp2}", off), "a stale fingerprint was shown:\n" + log()[off:]
        a.type_and_wait(f"/dcc close chat {nick}")

    def t_quit():
        for name in ("alice", "carol"):
            st[name].quit()
            assert st[name].sanitizer_reports() == [], st[name].sanitizer_reports()

    try:
        harness.main([t_install, t_start, t_schat_peer_listens, t_schat_alice_listens,
                      t_tls12_refused, t_no_downgrade, t_ssend_to_peer, t_ssend_from_peer,
                      t_passive_ssend, t_plain_send, t_erssi_to_erssi,
                      t_custom_cert_both_orders, t_custom_cert_broken_kept,
                      t_custom_cert_world_readable_refused, t_backup_not_deleted,
                      t_managed_world_readable_renewed, t_fifo_does_not_hang,
                      t_fingerprint_follows_live_cert, t_quit])
    finally:
        for name in ("alice", "carol"):
            if name in st:
                st[name].kill()
        if "bob" in st:
            st["bob"].close()
        if "ircd" in st:
            st["ircd"].stop()
        if not harness.keep_work():
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
