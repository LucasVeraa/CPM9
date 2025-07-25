#include "CPM.h"
#include "OpticFlowIO.h"

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>

#include <filesystem>
#include <dirent.h>   // Para listar archivos del directorio
#include <cstring>    // Para strcmp y strstr
#include <fstream>    // Para escritura de resultados en CSV

//para el calculo de tiempos
#include <sys/resource.h>
#include <time.h>
#include <sys/time.h>
#include "funciones.h"

//paralelo
#include <thread>
#include <chrono>

#include <omp.h>


using namespace std;


// draw each match as a 3x3 color block
void Match2Flow(FImage& inMat, FImage& ou, FImage& ov, int w, int h)
{
	if (!ou.matchDimension(w, h, 1)){
		ou.allocate(w, h, 1);
	}
	if (!ov.matchDimension(w, h, 1)){
		ov.allocate(w, h, 1);
	}
	ou.setValue(UNKNOWN_FLOW);
	ov.setValue(UNKNOWN_FLOW);
	int cnt = inMat.height();

	for (int i = 0; i < cnt; i++){
		float* p = inMat.rowPtr(i);
		float x = p[0];
		float y = p[1];
		float u = p[2] - p[0];
		float v = p[3] - p[1];
		for (int di = -1; di <= 1; di++){
			for (int dj = -1; dj <= 1; dj++){
				int tx = ImageProcessing::EnforceRange(x + dj, w);
				int ty = ImageProcessing::EnforceRange(y + di, h);
				ou[ty*w + tx] = u;
				ov[ty*w + tx] = v;
			}
		}
	}
}

void WriteMatches(const char *filename, FImage& inMat)
{

	int len = inMat.height();
	FILE *fid = fopen(filename, "w");

	for (int i = 0; i < len; i++){
		float x1 = inMat[4 * i + 0];
		float y1 = inMat[4 * i + 1];
		float x2 = inMat[4 * i + 2];
		float y2 = inMat[4 * i + 3];
		fprintf(fid, "%.0f %.0f %.0f %.0f\n", x1, y1, x2, y2);
		//fprintf(fid, "%.3f %.3f %.3f %.3f 1 100\n", x1, y1, x2, y2);
	}
	fclose(fid);
}

namespace fs = std::filesystem;

std::string removeExtension(const std::string& filename) {
    size_t lastDot = filename.find_last_of(".");
    return (lastDot == std::string::npos) ? filename : filename.substr(0, lastDot);
}



int main(int argc, char** argv)
{
	auto inicio = std::chrono::high_resolution_clock::now();
    if (argc < 3) {
        std::cerr << "USAGE: " << argv[0] << " <carpeta_imagenes> <archivo_salida_base>\n";
        return 1;
    }

    std::string carpeta = argv[1];
    std::string archivoSalidaBase = argv[2];

    std::vector<fs::path> imagenes;

    for (const auto& entrada : fs::directory_iterator(carpeta)) {
        if (entrada.is_regular_file()) {
            std::string ext = entrada.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tif") {
                imagenes.push_back(entrada.path());
            }
        }
    }

    if (imagenes.size() < 2) {
        std::cerr << "Debe haber al menos 2 imágenes válidas en la carpeta.\n";
        return 1;
    }

    // Cantidad de hilos
    const int numThreads = 8;
    omp_set_num_threads(numThreads);

    // Abrir archivos de salida por hilo e imprimir encabezados
    std::vector<std::ofstream> archivosSalida(numThreads);
    for (int t = 0; t < numThreads; ++t) {
        std::string nombreArchivo = archivoSalidaBase + "_thread_" + std::to_string(t) + ".csv";
        archivosSalida[t].open(nombreArchivo);
        if (!archivosSalida[t].is_open()) {
            std::cerr << "No se pudo abrir el archivo de salida: " << nombreArchivo << "\n";
            return 1;
        }
        archivosSalida[t] << "imagen1,imagen2,resultado,tiempo_ms\n";
    }

    std::cout << "Usando " << numThreads << " hilos.\n";

    // Paralelizar el doble for con OpenMP
    #pragma omp parallel for collapse(2) schedule(dynamic)
    for (int i = 0; i < (int)imagenes.size(); ++i) {
        for (int j = 0; j < (int)imagenes.size(); ++j) {
            int tid = omp_get_thread_num();
            auto start = std::chrono::high_resolution_clock::now();

            FImage img1, img2;
            img1.imread(imagenes[i].string().c_str());
            img2.imread(imagenes[j].string().c_str());

            if (img1.width() != img2.width() || img1.height() != img2.height()) {
                #pragma omp critical
                std::cerr << "Imágenes con diferentes dimensiones: "
                          << imagenes[i] << " y " << imagenes[j] << "\n";
                continue;
            }

            int w = img1.width();
            int h = img1.height();

            FImage matches, u, v;
            CPM cpm;
            cpm.SetStep(3);
            cpm.Matching(img1, img2, matches);
            Match2Flow(matches, u, v, w, h);

            double menor = std::numeric_limits<double>::max();
            double mayor = std::numeric_limits<double>::lowest();
            int conta1 = 0, conta2 = 0;

            for (int k = 0; k < w * h; ++k) {
                if (u.pData[k] != UNKNOWN_FLOW) {
                    double val = static_cast<double>(u.pData[k]);
                    menor = std::min(menor, val);
                    mayor = std::max(mayor, val);
                    conta1++;
                }
                if (v.pData[k] != UNKNOWN_FLOW) {
                    double val = static_cast<double>(v.pData[k]);
                    menor = std::min(menor, val);
                    mayor = std::max(mayor, val);
                    conta2++;
                }
            }

            double f = 0.0;
            if (conta1 == 0 || conta2 == 0) {
                #pragma omp critical
                std::cerr << "Flujo desconocido en todas las posiciones entre "
                          << imagenes[i] << " y " << imagenes[j] << "\n";
                f = 0.0;
            } else {
                int shift = static_cast<int>(std::abs(menor));
                int tam = static_cast<int>(std::fabs(menor) + std::fabs(mayor)) + 1;
                std::vector<int> hx(tam, 0), hy(tam, 0);

                for (int m = 0; m < w * h; ++m) {
                    if (u.pData[m] != UNKNOWN_FLOW) {
                        int bin = static_cast<int>(u.pData[m]) + shift;
                        if (bin >= 0 && bin < tam) hx[bin]++;
                    }
                    if (v.pData[m] != UNKNOWN_FLOW) {
                        int bin = static_cast<int>(v.pData[m]) + shift;
                        if (bin >= 0 && bin < tam) hy[bin]++;
                    }
                }

                double abajo = double(2 * conta1);
                for (int b = 0; b < tam; ++b) {
                    double arriba = double(hx[b] + hy[b]);
                    double resultado = arriba / abajo;
                    f += resultado * resultado;
                }
            }

            std::string nombre1 = removeExtension(imagenes[i].filename().string());
            std::string nombre2 = removeExtension(imagenes[j].filename().string());

            auto end = std::chrono::high_resolution_clock::now();
            double duracion = std::chrono::duration<double, std::milli>(end - start).count();

            // Escribir resultado en archivo correspondiente al hilo
            #pragma omp critical
            {
                archivosSalida[tid] << nombre1 << "," << nombre2 << "," << (std::isnan(f) ? 0.0 : f) << "," << duracion << "\n";
            }

            #pragma omp critical
            {
                std::cout << "Hilo " << tid << " comparó: " << nombre1 << " vs " << nombre2
                          << " = " << f << " en " << duracion << " ms\n";
            }
        }
    }

    // Cerrar archivos
    for (int t = 0; t < numThreads; ++t) {
        archivosSalida[t].close();
    }

    std::cout << "Comparaciones completadas.\n";
    auto fin = std::chrono::high_resolution_clock::now();  
    std::chrono::duration<double> duracion = fin - inicio;
    std::cout << " Tiempo total de ejecución: " << duracion.count() << " segundos.\n";
    return 0;
}
