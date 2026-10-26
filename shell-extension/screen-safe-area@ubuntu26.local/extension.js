// Keeps the desktop clear of the phone panel's camera cutout and rounded
// corners, the way Android's window insets do. The shape (in the current
// rotation) comes from Android through ubuntu26-native-network.
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';

Gio._promisify(Gio.DBusConnection.prototype, 'call');

const PHONE_CONNECTOR = 'Meta-0';
const SCREEN = 'local.ubuntu26.Display';
const SCREEN_PATH = '/local/ubuntu26/Display';
// Height of the panel's content (icons, clock) in theme pixels, for the
// corner clearance.
const PANEL_CONTENT_HEIGHT = 20;
// Style class from stylesheet.css.
const TRANSPARENT_PANEL = 'screen-safe-area-transparent';

/** Horizontal clearance a corner of `radius` needs at `y` from the edge. */
function cornerClearance(radius, y) {
    if (y >= radius)
        return 0;
    const d = radius - y;
    return radius - Math.sqrt(radius * radius - d * d);
}

export default class ScreenSafeArea extends Extension {
    enable() {
        this._struts = [];
        this._panelAdjusted = false;
        this._cancellable = new Gio.Cancellable();
        this._monitorsId = Main.layoutManager.connect('monitors-changed',
            () => this._queueUpdate());
        this._queueUpdate();
    }

    disable() {
        this._cancellable.cancel();
        if (this._updateId)
            GLib.source_remove(this._updateId);
        this._updateId = 0;
        Main.layoutManager.disconnect(this._monitorsId);
        this._reset();
    }

    _queueUpdate() {
        if (this._updateId)
            return;
        // Rotations and monitor changes arrive in bursts; settle first.
        this._updateId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 300, () => {
            this._updateId = 0;
            this._update().catch(e => console.warn(`screen-safe-area: ${e.message}`));
            return GLib.SOURCE_REMOVE;
        });
    }

    async _update() {
        const index = global.backend.get_monitor_manager()
            .get_monitor_for_connector(PHONE_CONNECTOR);
        const monitor = Main.layoutManager.monitors[index];
        if (!monitor) {
            this._reset();
            return;
        }
        const reply = await Gio.DBus.system.call(SCREEN, SCREEN_PATH, SCREEN, 'Refresh', null,
            new GLib.VariantType('(a{sv})'), Gio.DBusCallFlags.NONE, 5000, this._cancellable);
        const shape = reply.recursiveUnpack()[0];

        // Mid-rotation the two sides can disagree; the next change settles it.
        if ((monitor.width > monitor.height) !== (shape.Width > shape.Height))
            return;
        this._apply(index, monitor, shape);
    }

    _apply(index, monitor, shape) {
        this._reset();
        // Everything below is in stage pixels (physical, or logical with
        // fractional scaling); shape is in physical pixels.
        const toStage = monitor.width / shape.Width;
        const [top, right, bottom, left] = shape.SafeInsets.map(v => Math.ceil(v * toStage));
        const radius = shape.CornerRadius * toStage;
        let topCovered = 0;

        if (Main.layoutManager.primaryIndex === index) {
            // The panel keeps its row above the cutout and becomes a
            // transparent status bar over the wallpaper; the strut below
            // keeps windows clear of the rest of the cutout.
            Main.panel.add_style_class_name(TRANSPARENT_PANEL);

            // The panel lays out its boxes over its whole width (ignoring
            // its own padding), so pad the outer boxes past the corners.
            const themeScale = St.ThemeContext.get_for_stage(global.stage).scale_factor;
            const contentTop = (Main.panel.height - PANEL_CONTENT_HEIGHT * themeScale) / 2;
            const clearance = Math.ceil(cornerClearance(radius, contentTop) / themeScale);
            Main.panel._leftBox.set_style(`padding-left: ${clearance}px;`);
            Main.panel._rightBox.set_style(`padding-right: ${clearance}px;`);
            this._panelAdjusted = true;
            topCovered = Main.panel.height;
        }

        // Keep windows out of whatever the panel does not already cover.
        const {x, y, width, height} = monitor;
        if (top > topCovered)
            this._addStrut(x, y, width, top);
        if (bottom > 0)
            this._addStrut(x, y + height - bottom, width, bottom);
        if (left > 0)
            this._addStrut(x, y, left, height);
        if (right > 0)
            this._addStrut(x + width - right, y, right, height);
    }

    _addStrut(x, y, width, height) {
        // Not reactive, so it reserves space without catching input.
        const strut = new St.Widget({x, y, width, height, reactive: false});
        Main.layoutManager.addChrome(strut, {affectsStruts: true});
        this._struts.push(strut);
    }

    _reset() {
        for (const strut of this._struts) {
            Main.layoutManager.removeChrome(strut);
            strut.destroy();
        }
        this._struts = [];
        if (this._panelAdjusted) {
            Main.panel.remove_style_class_name(TRANSPARENT_PANEL);
            Main.panel._leftBox.set_style(null);
            Main.panel._rightBox.set_style(null);
            this._panelAdjusted = false;
        }
    }
}
