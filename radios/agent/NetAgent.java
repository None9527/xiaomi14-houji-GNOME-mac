package local.ubuntu26.connectivity;

import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.net.wifi.ScanResult;
import android.net.wifi.WifiConfiguration;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.graphics.Point;
import android.graphics.Rect;
import android.hardware.display.DisplayManager;
import android.os.Looper;
import android.os.Process;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.RoundedCorner;
import android.provider.Settings;
import android.telephony.SignalStrength;
import android.telephony.TelephonyManager;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Android owns all radio configuration, DHCP, routes, and credentials. This
 * agent reports Wi-Fi, mobile data, Bluetooth and airplane mode state to the
 * Ubuntu desktop and forwards its requests to the Android services.
 */
public final class NetAgent {
    private static void startup(String phase) {
        try (java.io.FileWriter out = new java.io.FileWriter("/data/local/ubuntu26/bridge/agent-startup.log", true)) {
            out.write(phase + "\n");
        } catch (Exception ignored) {}
    }
    private static Context context;
    private static WifiManager wifi;
    private static ConnectivityManager connectivity;
    private static BluetoothAdapter bluetooth;
    private static TelephonyManager telephony;
    private static final AtomicLong generation = new AtomicLong(1);

    private static void initialize() throws Exception {
        Class<?> runtimeClass = Class.forName("dalvik.system.VMRuntime");
        Object runtime = runtimeClass.getDeclaredMethod("getRuntime").invoke(null);
        runtimeClass.getDeclaredMethod("setHiddenApiExemptions", String[].class)
            .invoke(runtime, (Object) new String[]{"L"});
        if (Looper.myLooper() == null) Looper.prepareMainLooper();
        Class<?> threadClass = Class.forName("android.app.ActivityThread");
        // App processes register the mainline module service managers
        // (telephony, Bluetooth, ...) at startup; app_process does not.
        threadClass.getMethod("initializeMainlineModules").invoke(null);
        Object thread = threadClass.getMethod("systemMain").invoke(null);
        Context system = (Context) threadClass.getMethod("getSystemContext").invoke(thread);
        context = system.createPackageContext("com.android.shell", 0);
        wifi = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
        connectivity = (ConnectivityManager) context.getSystemService(Context.CONNECTIVITY_SERVICE);
        bluetooth = BluetoothAdapter.getDefaultAdapter();
        telephony = ((TelephonyManager) context.getSystemService(Context.TELEPHONY_SERVICE))
            .createForSubscriptionId(android.telephony.SubscriptionManager.getDefaultDataSubscriptionId());
        IntentFilter filter = new IntentFilter();
        filter.addAction(WifiManager.WIFI_STATE_CHANGED_ACTION);
        filter.addAction(WifiManager.SCAN_RESULTS_AVAILABLE_ACTION);
        filter.addAction(WifiManager.NETWORK_STATE_CHANGED_ACTION);
        filter.addAction(BluetoothAdapter.ACTION_STATE_CHANGED);
        filter.addAction(BluetoothAdapter.ACTION_LOCAL_NAME_CHANGED);
        filter.addAction(BluetoothDevice.ACTION_ACL_CONNECTED);
        filter.addAction(BluetoothDevice.ACTION_ACL_DISCONNECTED);
        filter.addAction(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
        filter.addAction(BluetoothDevice.ACTION_ALIAS_CHANGED);
        filter.addAction(Intent.ACTION_AIRPLANE_MODE_CHANGED);
        filter.addAction(ConnectivityManager.CONNECTIVITY_ACTION);
        context.registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context c, Intent intent) {
                generation.incrementAndGet();
            }
        }, filter, Context.RECEIVER_EXPORTED);
    }

    private static String unquote(String value) {
        if (value == null || value.equals("<unknown ssid>")) return "";
        return value.startsWith("\"") && value.endsWith("\"")
            ? value.substring(1, value.length() - 1) : value;
    }

    private static JSONObject snapshot() throws Exception {
        JSONObject result = new JSONObject();
        result.put("generation", generation.get());
        result.put("wifiEnabled", wifi.isWifiEnabled());
        WifiInfo info = wifi.getConnectionInfo();
        JSONObject current = new JSONObject();
        if (info != null) {
            current.put("ssid", unquote(info.getSSID()));
            current.put("bssid", info.getBSSID());
            current.put("networkId", info.getNetworkId());
            current.put("rssi", info.getRssi());
            current.put("frequency", info.getFrequency());
            current.put("linkSpeed", info.getLinkSpeed());
            current.put("state", info.getSupplicantState().name());
        }
        result.put("current", current);
        JSONArray aps = new JSONArray();
        for (ScanResult scan : wifi.getScanResults()) {
            JSONObject ap = new JSONObject();
            ap.put("ssid", scan.SSID);
            ap.put("bssid", scan.BSSID);
            ap.put("rssi", scan.level);
            ap.put("frequency", scan.frequency);
            ap.put("timestamp", scan.timestamp);
            ap.put("capabilities", scan.capabilities);
            aps.put(ap);
        }
        result.put("accessPoints", aps);
        JSONArray profiles = new JSONArray();
        List<WifiConfiguration> configured = wifi.getConfiguredNetworks();
        if (configured != null) for (WifiConfiguration config : configured) {
            JSONObject profile = new JSONObject();
            profile.put("networkId", config.networkId);
            profile.put("ssid", unquote(config.SSID));
            profile.put("keyManagement", config.allowedKeyManagement.toString());
            // Never return PSKs or other Android-owned secrets.
            profiles.put(profile);
        }
        result.put("profiles", profiles);
        JSONArray networks = new JSONArray();
        Network active = connectivity.getActiveNetwork();
        for (Network network : connectivity.getAllNetworks()) {
            LinkProperties link = connectivity.getLinkProperties(network);
            NetworkCapabilities caps = connectivity.getNetworkCapabilities(network);
            if (link == null || caps == null) continue;
            JSONObject entry = new JSONObject();
            entry.put("interface", link.getInterfaceName());
            entry.put("default", network.equals(active));
            entry.put("wifi", caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI));
            entry.put("cellular", caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR));
            entry.put("vpn", caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN));
            entry.put("internet", caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET));
            entry.put("validated", caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED));
            JSONArray addresses = new JSONArray();
            for (android.net.LinkAddress address : link.getLinkAddresses()) addresses.put(address.toString());
            entry.put("addresses", addresses);
            networks.put(entry);
        }
        result.put("networks", networks);
        // Content providers refuse this process (it is not an app), so read
        // the setting through the settings service command.
        putSafely(result, "airplane", () -> shellOutput("/system/bin/settings", "get", "global",
            Settings.Global.AIRPLANE_MODE_ON).trim().equals("1"));
        result.put("mobile", mobileSnapshot());
        result.put("bluetooth", bluetoothSnapshot());
        return result;
    }

    // Each field is read on its own: one refused query must not hide the rest.
    private interface Reader { Object read() throws Exception; }

    private static final JSONObject readErrors = new JSONObject();

    private static void putSafely(JSONObject object, String key, Reader reader) {
        try {
            object.put(key, reader.read());
        } catch (Exception error) {
            Throwable cause = error.getCause() == null ? error : error.getCause();
            try {
                readErrors.put(key, cause.getClass().getSimpleName() + ": " + cause.getMessage());
            } catch (Exception ignored) {}
        }
    }

    private static JSONObject mobileSnapshot() {
        JSONObject mobile = new JSONObject();
        if (telephony == null) return mobile;
        putSafely(mobile, "simReady", () -> telephony.getSimState() == TelephonyManager.SIM_STATE_READY);
        putSafely(mobile, "dataEnabled", () -> telephony.isDataEnabled());
        putSafely(mobile, "operatorName", () -> telephony.getNetworkOperatorName());
        putSafely(mobile, "operatorCode", () -> telephony.getNetworkOperator());
        putSafely(mobile, "networkType", () -> telephony.getDataNetworkType());
        putSafely(mobile, "signalLevel", () -> {
            SignalStrength strength = telephony.getSignalStrength();
            return strength == null ? 0 : strength.getLevel();
        });
        return mobile;
    }

    private static JSONObject bluetoothSnapshot() throws Exception {
        JSONObject result = new JSONObject();
        if (bluetooth == null) return result.put("present", false);
        result.put("present", true);
        putSafely(result, "state", () -> bluetooth.getState());
        putSafely(result, "name", () -> bluetooth.getName());
        putSafely(result, "address", () -> bluetooth.getAddress());
        JSONArray devices = new JSONArray();
        try {
            for (BluetoothDevice device : bluetooth.getBondedDevices()) {
                JSONObject entry = new JSONObject();
                entry.put("address", device.getAddress());
                putSafely(entry, "name", () -> device.getName());
                putSafely(entry, "alias", () -> device.getAlias());
                putSafely(entry, "class", () -> device.getBluetoothClass() == null
                    ? 0 : device.getBluetoothClass().hashCode());
                putSafely(entry, "connected",
                    () -> BluetoothDevice.class.getMethod("isConnected").invoke(device));
                devices.put(entry);
            }
        } catch (Exception ignored) {
            // Bonded devices are unavailable while Bluetooth is off.
        }
        result.put("devices", devices);
        return result;
    }

    /**
     * The panel's shape in its current rotation, in physical pixels: the
     * cutout's safe insets and areas, and the corner radius.
     */
    private static JSONObject displaySnapshot() throws Exception {
        Display display = ((DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE))
            .getDisplay(Display.DEFAULT_DISPLAY);
        Point size = new Point();
        display.getRealSize(size);
        JSONObject result = new JSONObject();
        result.put("rotation", display.getRotation());
        result.put("width", size.x);
        result.put("height", size.y);
        JSONArray insets = new JSONArray();
        JSONArray rects = new JSONArray();
        DisplayCutout cutout = display.getCutout();
        if (cutout != null) {
            insets.put(cutout.getSafeInsetTop()).put(cutout.getSafeInsetRight())
                .put(cutout.getSafeInsetBottom()).put(cutout.getSafeInsetLeft());
            for (Rect rect : cutout.getBoundingRects())
                rects.put(new JSONArray().put(rect.left).put(rect.top).put(rect.width()).put(rect.height()));
        } else {
            insets.put(0).put(0).put(0).put(0);
        }
        result.put("insets", insets);
        result.put("cutouts", rects);
        int radius = 0;
        for (int position = RoundedCorner.POSITION_TOP_LEFT; position <= RoundedCorner.POSITION_BOTTOM_LEFT; position++) {
            RoundedCorner corner = display.getRoundedCorner(position);
            if (corner != null) radius = Math.max(radius, corner.getRadius());
        }
        result.put("cornerRadius", radius);
        return result;
    }

    /** Runs an Android shell command (this agent runs as root) and returns its output. */
    private static String shellOutput(String... argv) throws Exception {
        java.lang.Process process = new ProcessBuilder(argv).redirectErrorStream(true).start();
        byte[] output = process.getInputStream().readAllBytes();
        if (!process.waitFor(12, TimeUnit.SECONDS) || process.exitValue() != 0)
            throw new IllegalStateException("Android command failed");
        return new String(output, StandardCharsets.UTF_8);
    }

    private static void shell(String... argv) throws Exception {
        shellOutput(argv);
    }

    private static boolean setBluetooth(boolean enabled) throws Exception {
        try {
            if (enabled ? bluetooth.enable() : bluetooth.disable()) return true;
        } catch (SecurityException ignored) {
            // The shell package identity may be refused; the service command is not.
        }
        shell("/system/bin/cmd", "bluetooth_manager", enabled ? "enable" : "disable");
        return true;
    }

    private static BluetoothDevice bondedDevice(String address) {
        for (BluetoothDevice device : bluetooth.getBondedDevices())
            if (device.getAddress().equalsIgnoreCase(address)) return device;
        throw new IllegalArgumentException("Unknown device");
    }

    /** Connects or disconnects every profile Android allows for the device. */
    private static boolean setDeviceConnected(String address, boolean connected) throws Exception {
        BluetoothDevice device = bondedDevice(address);
        Object status = BluetoothDevice.class.getMethod(connected ? "connect" : "disconnect").invoke(device);
        generation.incrementAndGet();
        return status instanceof Integer ? (Integer) status == 0 : Boolean.TRUE.equals(status);
    }

    private static void connect(JSONObject request) throws Exception {
        Class<?> listenerClass = Class.forName("android.net.wifi.WifiManager$ActionListener");
        CountDownLatch latch = new CountDownLatch(1);
        int[] error = new int[]{-1};
        Object listener = Proxy.newProxyInstance(listenerClass.getClassLoader(),
            new Class<?>[]{listenerClass}, (proxy, method, args) -> {
                if (method.getName().equals("hashCode")) return System.identityHashCode(proxy);
                if (method.getName().equals("equals")) return proxy == args[0];
                if (method.getName().equals("toString")) return "UbuntuNetworkActionListener";
                if (method.getName().equals("onSuccess")) {
                    error[0] = -1;
                    latch.countDown();
                } else if (method.getName().equals("onFailure")) {
                    error[0] = (Integer) args[0];
                    latch.countDown();
                }
                return null;
            });
        if (request.has("networkId")) {
            Method method = WifiManager.class.getMethod("connect", int.class, listenerClass);
            method.invoke(wifi, request.getInt("networkId"), listener);
        } else {
            String ssid = request.getString("ssid");
            String security = request.getString("security");
            String password = request.optString("password", "");
            if (ssid.isEmpty() || ssid.getBytes(StandardCharsets.UTF_8).length > 32
                || ssid.indexOf('\0') >= 0) throw new IllegalArgumentException("Invalid SSID");
            WifiConfiguration config = new WifiConfiguration();
            config.SSID = "\"" + ssid.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
            config.allowedKeyManagement.clear();
            if (security.equals("open")) {
                config.allowedKeyManagement.set(WifiConfiguration.KeyMgmt.NONE);
            } else if (security.equals("wpa2") || security.equals("wpa3")) {
                if (password.length() < 8 || password.length() > 63 || password.indexOf('\0') >= 0)
                    throw new IllegalArgumentException("Invalid WPA passphrase length");
                config.allowedKeyManagement.set(security.equals("wpa3")
                    ? WifiConfiguration.KeyMgmt.SAE : WifiConfiguration.KeyMgmt.WPA_PSK);
                config.preSharedKey = "\"" + password.replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
                WifiConfiguration.class.getField("requirePmf").setBoolean(config, security.equals("wpa3"));
            } else throw new IllegalArgumentException("Unsupported security type");
            Method method = WifiManager.class.getMethod("connect", WifiConfiguration.class, listenerClass);
            method.invoke(wifi, config, listener);
        }
        if (!latch.await(12, TimeUnit.SECONDS)) throw new IllegalStateException("Request acknowledgement timed out");
        if (error[0] >= 0) throw new IllegalStateException("Android rejected connection request: " + error[0]);
        // Acknowledgement is not association success. Consumers must observe state.
        generation.incrementAndGet();
    }

    private static JSONObject handle(JSONObject request) throws Exception {
        String action = request.getString("action");
        if (action.equals("snapshot")) return snapshot();
        if (action.equals("scan")) {
            boolean accepted = wifi.startScan();
            return new JSONObject().put("accepted", accepted);
        }
        if (action.equals("wifi")) {
            return new JSONObject().put("accepted", wifi.setWifiEnabled(request.getBoolean("enabled")));
        }
        if (action.equals("connect")) {
            connect(request);
            return new JSONObject().put("accepted", true);
        }
        if (action.equals("disconnect")) {
            return new JSONObject().put("accepted", wifi.disconnect());
        }
        if (action.equals("display")) return displaySnapshot();
        if (action.equals("bluetooth")) {
            return new JSONObject().put("accepted", setBluetooth(request.getBoolean("enabled")));
        }
        if (action.equals("bluetooth-name")) {
            return new JSONObject().put("accepted", bluetooth.setName(request.getString("name")));
        }
        if (action.equals("bluetooth-connect")) {
            return new JSONObject().put("accepted",
                setDeviceConnected(request.getString("address"), request.getBoolean("connected")));
        }
        if (action.equals("bluetooth-forget")) {
            BluetoothDevice device = bondedDevice(request.getString("address"));
            Object removed = BluetoothDevice.class.getMethod("removeBond").invoke(device);
            return new JSONObject().put("accepted", Boolean.TRUE.equals(removed));
        }
        if (action.equals("airplane")) {
            ConnectivityManager.class.getMethod("setAirplaneMode", boolean.class)
                .invoke(connectivity, request.getBoolean("enabled"));
            return new JSONObject().put("accepted", true);
        }
        if (action.equals("mobile-data")) {
            telephony.setDataEnabled(request.getBoolean("enabled"));
            return new JSONObject().put("accepted", true);
        }
        throw new IllegalArgumentException("Unsupported action");
    }

    private static void serve() throws Exception {
        startup("binding");
        try (LocalServerSocket server = new LocalServerSocket("ubuntu26-native-network-agent")) {
            startup("listening");
            while (true) {
                try (LocalSocket socket = server.accept()) {
                    if (socket.getPeerCredentials().getUid() != 0) continue;
                    socket.setSoTimeout(5000);
                    BufferedReader reader = new BufferedReader(new InputStreamReader(
                        socket.getInputStream(), StandardCharsets.UTF_8));
                    StringBuilder line = new StringBuilder();
                    int character;
                    while ((character = reader.read()) != -1 && character != '\n') {
                        if (line.length() >= 8192) throw new IllegalArgumentException("Request too large");
                        line.append((char) character);
                    }
                    JSONObject reply;
                    try {
                        reply = new JSONObject().put("ok", true).put("data", handle(new JSONObject(line.toString())));
                    } catch (Exception exception) {
                        Throwable cause = exception.getCause() == null ? exception : exception.getCause();
                        // No request contents or credentials in error output.
                        reply = new JSONObject().put("ok", false).put("error", cause.getClass().getSimpleName());
                    }
                    OutputStreamWriter writer = new OutputStreamWriter(socket.getOutputStream(), StandardCharsets.UTF_8);
                    writer.write(reply.toString() + "\n");
                    writer.flush();
                } catch (Exception ignored) {
                    // Malformed or disconnected client must not terminate the broker.
                }
            }
        }
    }

    public static void main(String[] args) throws Exception {
        startup("initializing");
        initialize();
        startup("initialized");
        if (args.length > 0 && args[0].equals("--probe")) {
            System.out.println(new JSONObject().put("uid", Process.myUid()).put("data", snapshot())
            .put("errors", readErrors));
            return;
        }
        Thread server = new Thread(() -> {
            try { serve(); } catch (Exception error) {
                System.err.println("Connectivity agent startup: " + error.getClass().getSimpleName());
                startup("server-error:" + error.getClass().getSimpleName());
                System.exit(1);
            }
        }, "ubuntu26-connectivity");
        server.setDaemon(true);
        server.start();
        startup("looping");
        Looper.loop();
        startup("loop-ended");
    }
}
