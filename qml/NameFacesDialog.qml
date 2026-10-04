import QtQuick
import QtQuick.Controls
import Omanta.Runtime

// Names one person and shows the other faces already grouped with them,
// checked. Uncheck a face that is someone else; those leave together as one
// new unnamed person.
Dialog {
    id: root

    property int personId: 0

    signal saved()

    anchors.centerIn: Overlay.overlay
    width: 480
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel
    closePolicy: Popup.CloseOnEscape

    function ask(personId, currentName) {
        root.personId = personId
        titleText.text = currentName === "" ? qsTr("Name this person") : qsTr("Rename")
        field.text = currentName
        field.selectAll()
        faceModel.clear()
        const faces = Faces.cluster(personId)
        for (let i = 0; i < faces.length; ++i) {
            faceModel.append({
                faceId: faces[i].faceId,
                crop: faces[i].crop,
                checked: true
            })
        }
        open()
        field.forceActiveFocus()
    }

    function commit() {
        const name = field.text.trim()
        if (name.length === 0)
            return
        const exclude = []
        let unchecked = 0
        for (let i = 0; i < faceModel.count; ++i) {
            const row = faceModel.get(i)
            if (row.checked)
                continue
            exclude.push(row.faceId)
            unchecked++
        }
        if (faceModel.count > 0 && unchecked === faceModel.count)
            return
        if (Faces.applyName(root.personId, name, exclude))
            root.saved()
    }

    Column {
        width: parent.width
        spacing: 10

        Text {
            id: titleText
            textFormat: Text.PlainText
            color: Colors.text
            font.pixelSize: 13
        }

        TextField {
            id: field

            width: parent.width
            color: Colors.text
            selectByMouse: true
            onAccepted: root.accept()
        }

        Text {
            visible: faceModel.count > 0
            width: parent.width
            wrapMode: Text.WordWrap
            text: qsTr("These faces are included. Uncheck one that is someone else.")
            color: Colors.textDim
            font.pixelSize: 12
        }

        Flickable {
            width: parent.width
            height: faceModel.count === 0 ? 0 : Math.min(contentHeight, 220)
            contentHeight: flow.implicitHeight
            clip: true

            Flow {
                id: flow
                width: parent.width
                spacing: 8

                Repeater {
                    model: faceModel
                    delegate: Item {
                        required property int index
                        required property var faceId
                        required property string crop
                        required property bool checked

                        width: 72
                        height: 72

                        Rectangle {
                            anchors.fill: parent
                            radius: 6
                            color: Colors.window
                            border.width: checked ? 2 : 1
                            border.color: checked ? Colors.accent : Colors.border
                            opacity: checked ? 1 : 0.45

                            Image {
                                anchors.fill: parent
                                anchors.margins: 3
                                source: crop
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: faceModel.setProperty(index, "checked", !checked)
                        }
                    }
                }
            }
        }
    }

    ListModel {
        id: faceModel
    }

    onAccepted: root.commit()
}
