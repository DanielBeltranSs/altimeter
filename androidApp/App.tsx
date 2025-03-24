import React, { useState } from 'react';
import { View, Text, Button, Alert, StyleSheet, ActivityIndicator } from 'react-native';
import * as DocumentPicker from 'expo-document-picker';
import * as FileSystem from 'expo-file-system';
import { Buffer } from 'buffer';
import { BleManager } from 'react-native-ble-plx';

// UUIDs del servicio y característica DFU en el ESP32
const DFU_SERVICE_UUID = "e3c0f200-3b0b-4253-9f53-3a351d8a146e";
const DFU_CHARACTERISTIC_UUID = "e3c0f201-3b0b-4253-9f53-3a351d8a146e";

// Nombre del dispositivo en modo DFU (ajusta según tu caso)
const TARGET_DEVICE_NAME = "ESP32-Altimetro-ota";

export default function App() {
  const [bleManager] = useState(new BleManager());
  const [updating, setUpdating] = useState(false);
  const [progress, setProgress] = useState(0);

  /**
   * Selecciona un archivo usando DocumentPicker.
   * Se configura el tipo como ANY para permitir la selección de cualquier archivo,
   * incluyendo archivos binarios (.bin). Se usa copyToCacheDirectory: true para asegurar su lectura.
   */
  const pickFirmwareFile = async (): Promise<string | null> => {
    try {
      const result = await DocumentPicker.getDocumentAsync({
        type: "*/*",
        copyToCacheDirectory: true,
      });
      console.log("Resultado DocumentPicker:", result);
      if (!result.canceled && result.assets && result.assets.length > 0) {
        return result.assets[0].uri;
      } else {
        return null;
      }
    } catch (error: any) {
      Alert.alert("Error", "No se pudo seleccionar el archivo: " + error.message);
      return null;
    }
  };

  /**
   * Lee el archivo en base64 y lo convierte a un Buffer.
   */
  const readFirmwareFile = async (uri: string): Promise<Buffer> => {
    try {
      const base64Data = await FileSystem.readAsStringAsync(uri, {
        encoding: FileSystem.EncodingType.Base64,
      });
      return Buffer.from(base64Data, 'base64');
    } catch (error: any) {
      throw new Error("Error leyendo el archivo: " + error.message);
    }
  };

  /**
   * Divide el Buffer en chunks de un tamaño dado.
   */
  const chunkFirmware = (buffer: Buffer, chunkSize: number): Buffer[] => {
    const chunks: Buffer[] = [];
    for (let i = 0; i < buffer.length; i += chunkSize) {
      chunks.push(buffer.slice(i, i + chunkSize));
    }
    return chunks;
  };

  /**
   * Escanea y conecta al dispositivo que contenga en su nombre TARGET_DEVICE_NAME.
   */
  const connectToDeviceForDFU = async (): Promise<any> => {
    return new Promise<any>((resolve, reject) => {
      console.log("Iniciando escaneo BLE...");
      bleManager.startDeviceScan(null, null, (error, device) => {
        if (error) {
          bleManager.stopDeviceScan();
          reject(error);
          return;
        }
        if (device && device.name && device.name.includes(TARGET_DEVICE_NAME)) {
          console.log("Dispositivo DFU encontrado:", device.name, device.id);
          bleManager.stopDeviceScan();
          device.connect()
            .then((connectedDevice) => connectedDevice.discoverAllServicesAndCharacteristics())
            .then((connectedDevice) => resolve(connectedDevice))
            .catch((err) => reject(err));
        }
      });
      setTimeout(() => {
        bleManager.stopDeviceScan();
        reject(new Error("Dispositivo DFU no encontrado en el tiempo esperado."));
      }, 15000);
    });
  };

  /**
   * Envía cada chunk al servicio DFU y, al final, la cadena "EOF".
   */
  const sendFirmware = async (device: any, chunks: Buffer[]): Promise<void> => {
    for (let i = 0; i < chunks.length; i++) {
      const chunk = chunks[i];
      const base64Chunk = chunk.toString('base64');
      await device.writeCharacteristicWithResponseForService(
        DFU_SERVICE_UUID,
        DFU_CHARACTERISTIC_UUID,
        base64Chunk
      );
      setProgress(Math.round(((i + 1) / chunks.length) * 100));
    }
    const eofBase64 = Buffer.from("EOF").toString('base64');
    await device.writeCharacteristicWithResponseForService(
      DFU_SERVICE_UUID,
      DFU_CHARACTERISTIC_UUID,
      eofBase64
    );
  };

  /**
   * Flujo principal de actualización de firmware.
   * Incluye negociación de MTU para acelerar la transferencia.
   */
  const updateFirmware = async () => {
    try {
      setUpdating(true);
      setProgress(0);
      // 1. Seleccionar archivo
      const fileUri = await pickFirmwareFile();
      if (!fileUri) {
        Alert.alert("Actualización", "No se seleccionó archivo de firmware");
        setUpdating(false);
        return;
      }
      // 2. Leer archivo y obtener buffer
      const firmwareBuffer = await readFirmwareFile(fileUri);
      console.log("Tamaño del firmware (bytes):", firmwareBuffer.length);
      // 3. Conectar al dispositivo DFU
      const dfuDevice = await connectToDeviceForDFU();
      console.log("Conectado a dispositivo DFU:", dfuDevice.id);
      // 4. Negociar un MTU mayor (por ejemplo, 247 bytes)
      const mtuResult = await dfuDevice.requestMTU(247);
      console.log("MTU negociado:", mtuResult);
      // Extraer el MTU negociado (suponiendo que el objeto tiene la propiedad 'mtu')
      const negotiatedMtu = mtuResult.mtu ? mtuResult.mtu : 247;
      // Definir el tamaño del chunk basado en el MTU (restando 3 bytes de overhead)
      const chunkSize = Math.min(200, negotiatedMtu - 3);
      console.log("Tamaño del chunk:", chunkSize);
      // 5. Dividir firmware en chunks
      const chunks = chunkFirmware(firmwareBuffer, chunkSize);
      console.log("Número de chunks:", chunks.length);
      // 6. Enviar firmware por BLE
      await sendFirmware(dfuDevice, chunks);
      Alert.alert("Actualización completada", "El dispositivo se reiniciará con el nuevo firmware.");
      setUpdating(false);
    } catch (error: any) {
      console.error("Error en actualización DFU:", error);
      Alert.alert("Error en actualización", error.message);
      setUpdating(false);
    }
  };

  return (
    <View style={styles.container}>
      <Text style={styles.title}>Actualización de Firmware</Text>
      {updating ? (
        <View style={styles.progressContainer}>
          <ActivityIndicator size="large" color="#007AFF" />
          <Text style={styles.progressText}>Progreso: {progress}%</Text>
        </View>
      ) : (
        <Button title="Actualizar Firmware" onPress={updateFirmware} />
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    padding: 20,
    justifyContent: 'center',
    backgroundColor: '#fff',
  },
  title: {
    fontSize: 22,
    textAlign: 'center',
    marginBottom: 20,
  },
  progressContainer: {
    alignItems: 'center',
  },
  progressText: {
    marginTop: 10,
    fontSize: 18,
  },
});
