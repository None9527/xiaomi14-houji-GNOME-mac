package com.genymobile.scrcpy;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.LocalSocketAddress;

import java.io.Closeable;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;

/**
 * Stand-in for scrcpy's device server. The stock scrcpy client pushes and
 * starts it (SCRCPY_SERVER_PATH=xscreen-scrcpy-server), and it relays the
 * client's streams to xscreend, which serves the GNOME desktop as a virtual
 * monitor. Each stream to xscreend starts with a line naming its role.
 */
public final class Server {
    private static final int XSCREEND_PORT = 7846;
    private static final String DEVICE_NAME = "GNOME";
    private static final int DEVICE_NAME_FIELD_LENGTH = 64;

    private Server() {
    }

    private static void fail(String message) {
        System.err.println("[server] ERROR: " + message);
        System.exit(1);
    }

    /** "WxH" from a --new-display value ("WxH", "WxH/dpi", "/dpi" or ""). */
    private static String requestedSize(String value) {
        if (value == null) {
            return null;
        }
        String size = value.split("/", 2)[0];
        return size.matches("\\d+x\\d+") ? size : null;
    }

    private static void relay(InputStream in, OutputStream out) {
        byte[] buffer = new byte[1 << 16];
        try {
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
                out.flush();
            }
        } catch (Exception ignored) {
            // The other side went away.
        }
        // Either side ending ends the session.
        System.exit(0);
    }

    public static void main(String... args) throws Exception {
        if (args.length < 1) {
            fail("missing client version");
        }
        // Packet framing changed in scrcpy 4.0 (session packets).
        int major = Integer.parseInt(args[0].replaceAll("^(\\d+).*", "$1"));
        if (major < 4) {
            fail("scrcpy " + args[0] + " is too old for XScreen; use scrcpy 4.0 or newer");
        }

        Map<String, String> options = new HashMap<>();
        for (int i = 1; i < args.length; i++) {
            int eq = args[i].indexOf('=');
            if (eq > 0) {
                options.put(args[i].substring(0, eq), args[i].substring(eq + 1));
            }
        }
        for (Map.Entry<String, String> option : options.entrySet()) {
            if (option.getKey().startsWith("list_") && "true".equals(option.getValue())) {
                System.out.println("[server] INFO: XScreen shows the GNOME desktop; there is nothing to list.");
                return;
            }
        }
        if ("false".equals(options.get("video")) || "camera".equals(options.get("video_source"))) {
            fail("XScreen streams the GNOME desktop; video must stay enabled");
        }

        List<String> roles = new ArrayList<>();
        roles.add("video");
        if (!"false".equals(options.get("audio"))) {
            roles.add("audio");
        }
        if (!"false".equals(options.get("control"))) {
            roles.add("control");
        }
        boolean forward = "true".equals(options.get("tunnel_forward"));
        String scid = options.get("scid");
        String socketName = scid == null ? "scrcpy" : "scrcpy_" + scid;
        String size = requestedSize(options.get("new_display"));
        // --show-touches: the pointer is drawn into the picture (needed
        // with --mouse=uhid, which hides the local one).
        boolean cursor = "true".equals(options.get("show_touches"));
        String session = UUID.randomUUID().toString().replace("-", "");

        // Client side: the adb tunnel, in the order the client expects.
        List<Closeable> open = new ArrayList<>();
        List<LocalSocket> clientSockets = new ArrayList<>();
        LocalServerSocket listener = forward ? new LocalServerSocket(socketName) : null;
        for (int i = 0; i < roles.size(); i++) {
            LocalSocket socket;
            if (forward) {
                socket = listener.accept();
                if (i == 0) {
                    // The client waits for this byte on its first socket
                    // before it opens the others.
                    socket.getOutputStream().write(0);
                    socket.getOutputStream().flush();
                }
            } else {
                socket = new LocalSocket();
                socket.connect(new LocalSocketAddress(socketName));
            }
            clientSockets.add(socket);
            open.add(socket);
        }
        if (listener != null) {
            listener.close();
        }

        OutputStream first = clientSockets.get(0).getOutputStream();
        byte[] name = new byte[DEVICE_NAME_FIELD_LENGTH];
        byte[] nameBytes = DEVICE_NAME.getBytes(StandardCharsets.UTF_8);
        System.arraycopy(nameBytes, 0, name, 0, nameBytes.length);
        first.write(name);
        first.flush();

        // Desktop side: one stream per role, video first (it starts the session).
        for (int i = 0; i < roles.size(); i++) {
            Socket desktop = new Socket("127.0.0.1", XSCREEND_PORT);
            desktop.setTcpNoDelay(true);
            open.add(desktop);
            String role = roles.get(i);
            String line = "XSCRCPY1 " + role + " " + session;
            if ("video".equals(role)) {
                line += (size != null ? " size=" + size : "") + (cursor ? " cursor" : "");
            }
            line += "\n";
            desktop.getOutputStream().write(line.getBytes(StandardCharsets.US_ASCII));
            desktop.getOutputStream().flush();

            LocalSocket client = clientSockets.get(i);
            new Thread(() -> {
                try {
                    relay(desktop.getInputStream(), client.getOutputStream());
                } catch (Exception e) {
                    System.exit(0);
                }
            }, role + "-down").start();
            new Thread(() -> {
                try {
                    relay(client.getInputStream(), desktop.getOutputStream());
                } catch (Exception e) {
                    System.exit(0);
                }
            }, role + "-up").start();
        }
    }
}
