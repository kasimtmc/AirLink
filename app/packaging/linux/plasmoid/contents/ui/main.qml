// AirLink in the panel, as an icon of its own: a click opens AirLink's tray panel at the icon
// (`airlink --panel X Y` hands the point to the running AirLink, or starts it).
import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasma5support as P5Support

PlasmoidItem {
    id: root
    preferredRepresentation: compactRepresentation
    // AirLink's state, as its tray icon shows it ($XDG_RUNTIME_DIR/airlink.status: the state, then
    // the tooltip; no file: AirLink isn't running).
    property string state: "off"
    property string detail: "AirLink çalışmıyor · tıklayınca başlar"
    // The third line counts what came (files, clipboard text): a change blinks the icon purple.
    property int received: -1
    property bool flash: false
    property int flashes: 0
    readonly property color stateColor: ({
        "connected": "#16A34A", "nodevices": "#D97706", "paused": "#6B7280", "error": "#DC2626", "starting": "#2563EB"
    })[state] ?? "#6B7280"

    toolTipMainText: "AirLink"
    toolTipSubText: detail

    compactRepresentation: Item {
        // A panel sizes its applets by their Layout hints.
        Layout.minimumWidth: Kirigami.Units.iconSizes.small
        Layout.minimumHeight: Kirigami.Units.iconSizes.small
        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
        Layout.preferredHeight: Kirigami.Units.iconSizes.medium

        // The disc in the state's color, AirLink's link mark on it (as the tray icon).
        Rectangle {
            id: disc
            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height) * 0.92
            height: width
            radius: width / 2
            color: root.flash ? "#9333EA" : root.stateColor
            opacity: root.state === "off" ? 0.6 : (area.containsMouse ? 1 : 0.95)
            Image {
                anchors.centerIn: parent
                width: parent.width * 0.62
                height: width
                source: Qt.resolvedUrl("../images/link.svg")
                sourceSize: Qt.size(64, 64)
            }
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {
                const at = area.mapToGlobal(area.width / 2, area.height / 2)
                // (the comment makes each command new: the engine runs a source it already has only once)
                launcher.run("'@AIRLINK_BIN@' --panel " + Math.round(at.x) + " " + Math.round(at.y) + " # " + Date.now())
            }
        }
    }
    // AirLink draws its panel itself; a Plasma applet without a full representation isn't shown at all.
    fullRepresentation: Item {
        Layout.minimumWidth: 1
        Layout.minimumHeight: 1
    }

    Timer {
        id: blink
        interval: 350
        repeat: true
        onTriggered: {
            root.flash = !root.flash
            if (--root.flashes <= 0) {
                root.flash = false
                stop()
            }
        }
    }

    P5Support.DataSource {
        id: status
        engine: "executable"
        connectedSources: ["cat \"$XDG_RUNTIME_DIR/airlink.status\" 2>/dev/null"]
        interval: 1000
        onNewData: (source, data) => {
            const lines = (data["stdout"] || "").split("\n")
            root.state = lines[0] ? lines[0] : "off"
            const count = parseInt(lines[2] || "")
            if (!isNaN(count)) {
                if (root.received >= 0 && count > root.received) {
                    root.flashes = 6
                    blink.restart()
                }
                root.received = count
            }
            root.detail = lines[0] ? (lines[1] || "AirLink") : "AirLink çalışmıyor · tıklayınca başlar"
        }
    }

    P5Support.DataSource {
        id: launcher
        engine: "executable"
        connectedSources: []
        onNewData: (source, data) => disconnectSource(source)
        function run(command) { connectSource(command) }
    }
}
