import SwiftUI

struct PresetBar: View {
    @ObservedObject var connection: EmulatorConnection
    @State private var namingOperation: UInt32 = 0
    @State private var proposedName = ""
    @State private var showName = false
    @State private var showDelete = false
    @State private var pendingSwitch: Int?
    @State private var showDiscard = false

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: "slider.horizontal.3").foregroundStyle(.cyan)
            Menu {
                Section("Bundled") {
                    ForEach(connection.presets.entries.filter { !$0.isUser }) { preset in
                        Button(preset.name) { select(preset.id) }
                    }
                }
                Section("Your presets") {
                    ForEach(connection.presets.entries.filter(\.isUser)) { preset in
                        Button(preset.name) { select(preset.id) }
                    }
                }
            } label: {
                Text(connection.presets.active?.name ?? "Unsaved setup")
                    .font(.headline).frame(minWidth: 170, alignment: .leading)
            }.menuStyle(.borderlessButton).frame(maxWidth: 330)
            if connection.presets.modified {
                Text("Edited").font(.caption).foregroundStyle(.orange)
            }
            Spacer()
            Button("Save") { connection.presetCommand(2) }
                .disabled(connection.presets.active?.isUser != true || !connection.presets.modified)
            Button("Save As…") { name(operation: 3) }
            Menu {
                Button("Rename…") { name(operation: 4) }
                Button("Delete…", role: .destructive) { showDelete = true }
            } label: { Image(systemName: "ellipsis.circle") }
                .menuStyle(.borderlessButton).frame(width: 28)
                .disabled(connection.presets.active?.isUser != true)
        }
        .disabled(connection.state != .connected || connection.presetBusy)
        .padding(.horizontal, 26).padding(.vertical, 13)
        .sheet(isPresented: $showName) {
            VStack(alignment: .leading, spacing: 18) {
                Text(namingOperation == 3 ? "Save a new preset" : "Rename preset").font(.headline)
                TextField("Preset name", text: $proposedName).textFieldStyle(.roundedBorder)
                HStack {
                    Spacer()
                    Button("Cancel") { showName = false }.keyboardShortcut(.cancelAction)
                    Button("Save") {
                        connection.presetCommand(namingOperation, name: proposedName.trimmingCharacters(in: .whitespacesAndNewlines))
                        showName = false
                    }.keyboardShortcut(.defaultAction)
                        .disabled(proposedName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || proposedName.utf8.count > 100)
                }
            }.padding(24).frame(width: 400)
        }
        .alert("Delete this user preset?", isPresented: $showDelete) {
            Button("Cancel", role: .cancel) {}
            Button("Delete", role: .destructive) { connection.presetCommand(5) }
        } message: { Text("Its preset file will be removed. The current picture settings will stay active as an unsaved setup.") }
        .alert("Discard unsaved edits?", isPresented: $showDiscard) {
            Button("Cancel", role: .cancel) { pendingSwitch = nil }
            Button("Switch preset", role: .destructive) {
                if let id = pendingSwitch { connection.presetCommand(1, id: id) }
                pendingSwitch = nil
            }
        } message: { Text("Use Save or Save As to keep these settings before switching.") }
        .alert("Preset operation failed", isPresented: Binding(get: { connection.presetError != nil }, set: { if !$0 { connection.presetError = nil } })) {
            Button("OK") { connection.presetError = nil }
        } message: { Text(connection.presetError ?? "") }
    }
    private func select(_ id: Int) {
        if connection.presets.modified { pendingSwitch = id; showDiscard = true }
        else { connection.presetCommand(1, id: id) }
    }
    private func name(operation: UInt32) {
        namingOperation = operation
        proposedName = connection.presets.active?.name ?? "My CRT"
        if operation == 3, connection.presets.active != nil { proposedName += " — Custom" }
        showName = true
    }
}
