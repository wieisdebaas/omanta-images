import QtQuick
import QtQuick.Controls
import Omanta.Runtime

Item {
    id: root

    required property var tab
    property alias currentIndex: view.currentIndex
    readonly property int tileSize: 220
    readonly property int cellWidth: tileSize + 16
    readonly property int cellHeight: tileSize + 42
    readonly property int columns: Math.max(1, Math.floor(view.width / cellWidth))

    property string activePath: ""
    property string activeName: ""
    property string activeThumbnail: ""

    function positionAt(row) { view.positionViewAtIndex(row, GridView.Contain); }

    GridView {
        id: view

        anchors.fill: parent
        anchors.margins: 8
        model: root.tab.files
        cellWidth: root.cellWidth
        cellHeight: root.cellHeight
        clip: true
        focus: true
        keyNavigationEnabled: false
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true

        readonly property real prefetchDistance:
            height * (Math.abs(verticalVelocity) > 2200 ? 3 : 1.5)
        displayMarginBeginning: verticalVelocity < -100 ? prefetchDistance : height * 0.75
        displayMarginEnd: verticalVelocity > 100 ? prefetchDistance : height * 0.75

        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: cell

            required property int index
            required property string name
            required property string displayName
            required property string filePath
            required property real size
            required property var modified

            width: view.cellWidth
            height: view.cellHeight

            readonly property bool inViewport:
                y + height >= view.contentY && y <= view.contentY + view.height
            readonly property int requestPriority: inViewport ? 100 : 10
            readonly property string thumbnailSource:
                Thumbnails.photoSource(filePath, modified, size, root.tileSize,
                                       requestPriority)

            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                radius: Colors.radius
                color: root.tab.isSelected(cell.name) ? Colors.selection
                     : mouse.containsMouse ? Colors.hover : Colors.chrome
            }

            Rectangle {
                x: 8
                y: 8
                width: root.tileSize
                height: root.tileSize
                color: Colors.window

                Image {
                    anchors.fill: parent
                    anchors.margins: 2
                    source: cell.thumbnailSource
                    sourceSize: Qt.size(root.tileSize, root.tileSize)
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    cache: false
                }
            }

            Text {
                textFormat: Text.PlainText
                x: 8
                y: root.tileSize + 13
                width: root.tileSize
                text: cell.displayName
                color: root.tab.isSelected(cell.name) ? Colors.selectionText : Colors.text
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideMiddle
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                onClicked: {
                    root.tab.setCurrent(cell.index, false)
                    root.activePath = cell.filePath
                    root.activeName = cell.displayName
                    root.activeThumbnail = cell.thumbnailSource
                    preview.open()
                }
            }
        }
    }

    Popup {
        id: preview

        parent: Overlay.overlay
        x: 0
        y: 0
        width: Overlay.overlay ? Overlay.overlay.width : 0
        height: Overlay.overlay ? Overlay.overlay.height : 0
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: {
            root.activePath = ""
            root.activeName = ""
            root.activeThumbnail = ""
        }

        background: Rectangle { color: Colors.window }

        contentItem: Item {
            Image {
                anchors.fill: parent
                anchors.margins: 24
                source: preview.opened ? root.activeThumbnail : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
            }

            Image {
                id: fullImage
                anchors.fill: parent
                anchors.margins: 24
                source: preview.opened ? Thumbnails.originalSource(root.activePath) : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                opacity: status === Image.Ready ? 1 : 0
            }

            Text {
                textFormat: Text.PlainText
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 16
                text: root.activeName
                color: Colors.text
                font.pixelSize: 13
            }

            DialogCloseButton {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 12
                onTriggered: preview.close()
            }
        }
    }
}