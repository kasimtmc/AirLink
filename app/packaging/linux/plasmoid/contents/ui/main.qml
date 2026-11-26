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
    toolTipMainText: "AirLink"
    toolTipSubText: "Cihazlar, medya, mesajlar ve aramalar"

    compactRepresentation: Item {
        // A panel sizes its applets by their Layout hints.
        Layout.minimumWidth: Kirigami.Units.iconSizes.small
        Layout.minimumHeight: Kirigami.Units.iconSizes.small
        Layout.preferredWidth: Kirigami.Units.iconSizes.medium
        Layout.preferredHeight: Kirigami.Units.iconSizes.medium

        Kirigami.Icon {
            anchors.fill: parent
            source: "airlink"  // installed with the app (install.sh)
            active: area.containsMouse
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

    P5Support.DataSource {
        id: launcher
        engine: "executable"
        connectedSources: []
        onNewData: (source, data) => disconnectSource(source)
        function run(command) { connectSource(command) }
    }
}
