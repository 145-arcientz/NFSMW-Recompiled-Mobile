import MemoriaNativa
import SwiftUI

struct ContentView: View {
    @State private var informe = "Pulsa Probar."
    @State private var probando = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Button(probando ? "Probando..." : "Probar") { probar() }
                .buttonStyle(.borderedProminent)
                .disabled(probando)
            ScrollView {
                Text(informe)
                    .font(.system(.footnote, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding()
    }

    private func probar() {
        probando = true
        let tmp = NSTemporaryDirectory()
        Task.detached {
            var bufer = [CChar](repeating: 0, count: 16384)
            nfsmw_prueba_memoria(tmp, &bufer, bufer.count)
            let texto = String(cString: bufer)
            // Tambien a Documentos, para sacarlo con la app Archivos.
            let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            try? texto.write(to: docs.appendingPathComponent("prueba_memoria.txt"), atomically: true,
                             encoding: .utf8)
            await MainActor.run {
                informe = texto
                probando = false
            }
        }
    }
}
