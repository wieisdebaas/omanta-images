import QtQuick
import QtQuick.Controls
import Omanta.Runtime

// One row of face chips above the Ctrl+3 grid. Click a face to keep only
// that person's photos. Click the caption to type a name.
Item {
    id: root

    property bool active: false
    property string emptyText: qsTr("No faces in this folder")

    signal personClicked(int personId)
    signal nameRequested(int personId, string currentName)

    function openChipMenu(personId, stranger) {
        if (stranger)
            return
        chipMenu.personId = personId
        chipMenu.popup()
    }

    implicitHeight: active ? 112 : 0
    height: implicitHeight
    visible: active
    clip: true

    readonly property string statusText: active ? Faces.status : ""

    ListView {
        id: chips

        anchors.fill: parent
        anchors.bottomMargin: message.visible || actions.implicitHeight > 0 ? 28 : 0
        orientation: ListView.Horizontal
        spacing: 8
        leftMargin: 12
        rightMargin: 12
        topMargin: 8
        model: root.active ? Faces.people : null
        visible: root.active && count > 0

        delegate: Item {
            id: chip

            required property int index
            required property var personId
            required property string name
            required property string label
            required property string crop
            required property bool named
            required property bool stranger
            required property int photoCount

            width: 76
            height: chips.height - 8

            readonly property bool selected: {
                if (!root.active)
                    return false
                const ids = Faces.selectedPersonIds
                const mine = Number(chip.personId)
                for (let i = 0; i < ids.length; ++i) {
                    if (Number(ids[i]) === mine)
                        return true
                }
                return false
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 68
                spacing: 4

                Rectangle {
                    width: 64
                    height: 64
                    radius: 6
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: Colors.window
                    border.width: chip.selected ? 2 : 1
                    border.color: chip.selected ? Colors.accent
                                 : chip.named ? Colors.border : Colors.textDim

                    Image {
                        anchors.fill: parent
                        anchors.margins: 3
                        source: chip.crop
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        visible: !chip.stranger && chip.crop !== ""
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: chip.stranger
                        text: "?"
                        color: Colors.text
                        font.pixelSize: 28
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        cursorShape: Qt.PointingHandCursor
                        onClicked: mouse => {
                            if (mouse.button === Qt.RightButton)
                                root.openChipMenu(chip.personId, chip.stranger)
                            else
                                root.personClicked(chip.personId)
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: chip.label
                    color: chip.selected ? Colors.accent : Colors.text
                    font.pixelSize: 11
                    font.italic: !chip.named && !chip.stranger
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        cursorShape: Qt.PointingHandCursor
                        onClicked: mouse => {
                            if (mouse.button === Qt.RightButton)
                                root.openChipMenu(chip.personId, chip.stranger)
                            else if (!chip.stranger)
                                root.nameRequested(chip.personId, chip.name)
                            else
                                root.personClicked(chip.personId)
                        }
                    }
                }
            }
        }
    }

    Text {
        id: message

        anchors.left: parent.left
        anchors.right: actions.left
        anchors.bottom: parent.bottom
        anchors.margins: 8
        visible: root.active && (chips.count === 0 || root.statusText !== "")
        text: root.statusText !== "" ? root.statusText
             : root.emptyText
        color: Colors.textDim
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    Row {
        id: actions

        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8
        spacing: 12

        Text {
            visible: root.active && Faces.selectedPersonIds.length === 2
            text: qsTr("Merge")
            color: Colors.accent
            font.pixelSize: 12

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: Faces.mergeSelected()
            }
        }

        Text {
            visible: root.active && (Faces.indexing || Faces.paused)
            text: !root.active ? "" : (Faces.paused ? qsTr("Resume") : qsTr("Pause"))
            color: Colors.accent
            font.pixelSize: 12

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: Faces.setPaused(!Faces.paused)
            }
        }
    }

    Menu {
        id: chipMenu

        property int personId: 0

        MenuItem {
            text: qsTr("Mark as stranger")
            onTriggered: {
                if (root.active)
                    Faces.markAsStranger(chipMenu.personId)
            }
        }
    }
}
