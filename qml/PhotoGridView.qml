import QtQuick
import QtQuick.Controls
import Omanta.Runtime

// Large-tile photo browser (Ctrl+3). Virtualized GridView + async photo
// thumbnails (disk/RAM cache, priority by viewport).
Item {
    id: root

    required property var tab
    property alias currentIndex: view.currentIndex
    readonly property int tileSize: Math.max(160, root.tab.zoom * 2)
    readonly property int cellWidth: tileSize + 24
    readonly property int cellHeight: tileSize + 48
    readonly property int columns: Math.max(1, Math.floor(view.width / cellWidth))
    readonly property bool slowStorage: Thumbnails.isSlowStorage(root.tab.path)

    property string activePath: ""
    property string activeName: ""
    property var activeModified
    property real activeSize: 0

    function positionAt(row) { view.positionViewAtIndex(row, GridView.Contain); }

    Component.onCompleted: {
        Thumbnails.bindPhotoIo(root, root.slowStorage)
        syncFaces()
    }
    Component.onDestruction: {
        if (root.faceScrollHold)
            Faces.setScrolling(false)
        if (Settings.showFaces || root.facesLibrary || root.facesWatching)
            Faces.watch("", false, false)
    }
    onSlowStorageChanged: Thumbnails.bindPhotoIo(root, root.slowStorage)

    // facesWatching remembers that we already constructed the index, so
    // closing the layer can stop it without opening the database on every
    // Ctrl+3 when the button is off.
    property bool facesWatching: false
    property bool facesLibrary: false
    property bool faceScrollHold: false

    function syncFaces() {
        if (root.tab.path === "people:///") {
            if (root.faceScrollHold) {
                root.faceScrollHold = false
                Faces.setScrolling(false)
            }
            Faces.openLibrary()
            root.facesLibrary = true
            root.facesWatching = false
            return
        }
        const on = Settings.showFaces && root.tab.viewMode === "photo"
                && Platform.isLocal(root.tab.path)
        if (!on) {
            if (root.facesWatching || root.facesLibrary) {
                if (root.faceScrollHold) {
                    root.faceScrollHold = false
                    Faces.setScrolling(false)
                }
                Faces.watch("", false, false)
                root.facesWatching = false
                root.facesLibrary = false
            }
            return
        }
        root.facesLibrary = false
        Faces.watch(root.tab.path, root.tab.showHidden, true)
        root.facesWatching = true
    }

    Connections {
        target: root.tab
        function onPathChanged() { root.syncFaces() }
        function onShowHiddenChanged() { root.syncFaces() }
    }

    Connections {
        target: Settings
        function onChanged() { root.syncFaces() }
    }

    PeopleBar {
        id: peopleBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        // The bar itself is what first touches the face index. Leaving it
        // hidden must not construct Faces.
        active: root.tab.path === "people:///"
             || (Settings.showFaces && Platform.isLocal(root.tab.path))
        emptyText: root.tab.path === "people:///"
            ? qsTr("No named people yet. Name someone in a folder to list them here.")
            : qsTr("No faces in this folder")
        onPersonClicked: personId => Faces.togglePerson(personId)
        onNameRequested: (personId, currentName) => nameDialog.ask(personId, currentName)
    }

    function faceSpot(mark, image) {
        const iw = mark.imageWidth
        const ih = mark.imageHeight
        if (!image || iw <= 0 || ih <= 0 || image.width <= 0)
            return Qt.rect(0, 0, 0, 0)
        const scale = Math.min(image.width / iw, image.height / ih)
        const drawnW = iw * scale
        const drawnH = ih * scale
        const ox = image.x + (image.width - drawnW) / 2
        const oy = image.y + (image.height - drawnH) / 2
        return Qt.rect(ox + mark.x * scale, oy + mark.y * scale,
                       mark.width * scale, mark.height * scale)
    }

    function askName(personId, currentName) {
        nameDialog.ask(personId, currentName)
    }

    NameFacesDialog {
        id: nameDialog
    }

    GridView {
        id: view

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.top: peopleBar.bottom
        anchors.margins: 8
        model: root.tab.files
        cellWidth: root.cellWidth
        cellHeight: root.cellHeight
        clip: true
        focus: true
        keyNavigationEnabled: false
        boundsBehavior: Flickable.StopAtBounds

        // Prefetch less on SMB/removable disks — random seeks thrash a HDD.
        readonly property real prefetchDistance: {
            if (root.slowStorage)
                return height * (Math.abs(verticalVelocity) > 2200 ? 1.0 : 0.5)
            return height * (Math.abs(verticalVelocity) > 2200 ? 3 : 1.5)
        }
        displayMarginBeginning: verticalVelocity < -100 ? prefetchDistance
                                 : height * (root.slowStorage ? 0.35 : 1.0)
        displayMarginEnd: verticalVelocity > 100 ? prefetchDistance
                            : height * (root.slowStorage ? 0.35 : 1.0)

        onVerticalVelocityChanged: {
            if (!root.facesWatching)
                return
            const flying = Math.abs(verticalVelocity) > 2200
            if (flying === root.faceScrollHold)
                return
            root.faceScrollHold = flying
            Faces.setScrolling(flying)
        }

        ScrollBar.vertical: ScrollBar {}

        delegate: Item {
            id: cell

            required property int index
            required property string name
            required property string displayName
            required property string filePath
            required property string targetPath
            required property string contentType
            required property real size
            required property var modified

            width: view.cellWidth
            height: view.cellHeight

            readonly property string previewPath:
                cell.targetPath !== "" ? cell.targetPath : cell.filePath
            readonly property bool inViewport:
                y + height >= view.contentY && y <= view.contentY + view.height
            readonly property bool nearViewport:
                y + height >= view.contentY - view.height
                && y <= view.contentY + view.height * 2
            // Thread-pool priority only (not part of Image.source).
            readonly property int requestPriority:
                inViewport ? 100 : nearViewport ? 50 : 10
            readonly property string thumbnailSource:
                Thumbnails.photoSource(previewPath, cell.modified, cell.size,
                                       root.tileSize, requestPriority)

            Rectangle {
                anchors.fill: parent
                anchors.margins: 4
                radius: Colors.radius
                color: root.tab.isSelected(cell.name) ? Colors.selection
                     : mouse.containsMouse ? Colors.hover : Colors.chrome
            }

            Column {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 10
                spacing: 8
                width: root.tileSize

                Rectangle {
                    width: root.tileSize
                    height: root.tileSize
                    color: Colors.window
                    radius: 2

                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: cell.thumbnailSource
                        sourceSize: Qt.size(root.tileSize, root.tileSize)
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        // Versioned URLs already invalidate on edit; caching
                        // makes scroll-back instant without re-hitting the provider.
                        cache: true
                    }
                }

                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    text: cell.displayName
                    color: root.tab.isSelected(cell.name) ? Colors.selectionText : Colors.text
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideMiddle
                }
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: mouse => {
                    root.tab.setCurrent(cell.index, mouse.modifiers & Qt.ShiftModifier)
                    if (mouse.button === Qt.RightButton) {
                        root.tab.requestContextMenu()
                        return
                    }
                    root.activePath = cell.previewPath
                    root.activeName = cell.displayName
                    root.activeModified = cell.modified
                    root.activeSize = cell.size
                    preview.open()
                }
                onDoubleClicked: root.tab.activate(cell.index)
            }
        }
    }

    Popup {
        id: preview

        anchors.centerIn: Overlay.overlay
        width: Overlay.overlay ? Math.min(Overlay.overlay.width - 48, 1200) : 800
        height: Overlay.overlay ? Math.min(Overlay.overlay.height - 48, 900) : 600
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        onClosed: {
            root.activePath = ""
            root.activeName = ""
            root.activeModified = undefined
            root.activeSize = 0
        }

        background: Rectangle {
            color: Colors.window
            border.color: Colors.border
            radius: Colors.radius
        }

        contentItem: Item {
            Image {
                id: previewImage

                anchors.fill: parent
                anchors.margins: 24
                anchors.bottomMargin: 48
                // A NAS photo is an smb:// URI. Qt cannot open that as a file
                // URL, so the popup asks the photo provider, which reads it
                // through GIO. Local files still show the original, and with
                // faces on that original is oriented the same way the boxes are.
                source: !preview.opened || root.activePath === "" ? ""
                      : (Settings.showFaces || root.tab.path === "people:///")
                        && Platform.isLocal(root.activePath)
                        ? Faces.previewSource(root.activePath)
                      : Platform.isLocal(root.activePath)
                        ? Thumbnails.originalSource(root.activePath)
                        : Thumbnails.photoSource(root.activePath, root.activeModified,
                                                 root.activeSize, 1024, 100)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
            }

            Repeater {
                model: {
                    const show = preview.opened && Platform.isLocal(root.activePath)
                            && (Settings.showFaces || root.tab.path === "people:///")
                    if (!show)
                        return []
                    const rev = Faces.revision
                    void rev
                    return Faces.facesOn(root.activePath)
                }
                delegate: Rectangle {
                    required property var modelData

                    readonly property rect spot: root.faceSpot(modelData, previewImage)
                    x: spot.x
                    y: spot.y
                    width: Math.max(0, spot.width)
                    height: Math.max(0, spot.height)
                    color: "transparent"
                    border.color: "#ffffff"
                    border.width: 2
                    radius: 4

                    Text {
                        anchors.left: parent.left
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 2
                        visible: modelData.stranger || modelData.name !== ""
                        text: modelData.stranger ? qsTr("Stranger") : modelData.name
                        color: "#ffffff"
                        font.pixelSize: 12
                        style: Text.Outline
                        styleColor: "#000000"
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.bottom
                        anchors.topMargin: 2
                        text: qsTr("Not this person")
                        color: "#ffffff"
                        font.pixelSize: 11
                        style: Text.Outline
                        styleColor: "#000000"

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Faces.detachFace(modelData.faceId)
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (modelData.stranger)
                                return
                            root.askName(modelData.personId, modelData.name)
                        }
                    }
                }
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
                dialog: preview
            }
        }
    }
}
