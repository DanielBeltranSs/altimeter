import React, { useState } from 'react';
import { View, Text, Button, Alert, StyleSheet, ActivityIndicator } from 'react-native';
import * as DocumentPicker from 'expo-document-picker';
import * as FileSystem from 'expo-file-system';
import { Buffer } from 'buffer';
import { BleManager } from 'react-native-ble-plx';

// UUIDs del servicio y característica DFU en el ESP32
const DFU_SERVICE_UUID = "e3c0f200-3b0b-4253-9f53-3a351d8a146e";
const DFU_CHARACTERISTIC_UUID = "e3c0f201-3b0b-4253-9f53-3a351d8a146e";
// Nombre del dispositivo en modo DFU
const TARGET_DEVICE_NAME = "ESP32-Altimetro-ota";

export default function FirmwareUpdate({ onBack }) {
  const [bleManager] = useState(new BleManager());
  const [updating, setUpdating] = useState(false);
  const [progress, setProgress] = useState(0);

  // Función para seleccionar el archivo de firmware.
  const pickFirmwareFile = async () => {
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
    } catch (error) {
      Alert.alert("Error", "No se pudo seleccionar el archivo: " + error.message);
      return null;
    }
  };

  // Función para leer el archivo en base64 y convertirlo a un Buffer.
  const readFirmwareFile = async (uri) => {
    try {
      const base64Data = await FileSystem.readAsStringAsync(uri, {
        encoding: FileSystem.EncodingType.Base64,
      });
      return Buffer.from(base64Data, 'base64');
    } catch (error) {
      throw new Error("Error leyendo el archivo: " + error.message);
    }
  };

  // Divide el Buffer en chunks de un tamaño dado.
  const chunkFirmware = (buffer, chunkSize) => {
    const chunks = [];
    for (let i = 0; i < buffer.length; i += chunkSize) {
      chunks.push(buffer.slice(i, i + chunkSize));
    }
    return chunks;
  };

  // Escanea y conecta al dispositivo cuyo nombre contenga TARGET_DEVICE_NAME.
  const connectToDeviceForDFU = async () => {
    return new Promise((resolve, reject) => {
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

  // Envía cada chunk al servicio DFU y, al final, la cadena "EOF".
  const sendFirmware = async (device, chunks) => {
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

  // Flujo principal de actualización de firmware, que incluye negociación de MTU.
  const updateFirmware = async () => {
    try {
      setUpdating(true);
      setProgress(0);
      const fileUri = await pickFirmwareFile();
      if (!fileUri) {
        Alert.alert("Actualización", "No se seleccionó archivo de firmware");
        setUpdating(false);
        return;
      }
      const firmwareBuffer = await readFirmwareFile(fileUri);
      console.log("Tamaño del firmware (bytes):", firmwareBuffer.length);
      const dfuDevice = await connectToDeviceForDFU();
      console.log("Conectado a dispositivo DFU:", dfuDevice.id);
      const mtuResult = await dfuDevice.requestMTU(247);
      console.log("MTU negociado:", mtuResult);
      const negotiatedMtu = mtuResult.mtu ? mtuResult.mtu : 247;
      const chunkSize = Math.min(200, negotiatedMtu - 3);
      console.log("Tamaño del chunk:", chunkSize);
      const chunks = chunkFirmware(firmwareBuffer, chunkSize);
      console.log("Número de chunks:", chunks.length);
      await sendFirmware(dfuDevice, chunks);
      Alert.alert("Actualización completada", "El dispositivo se reiniciará con el nuevo firmware.");
      setUpdating(false);
    } catch (error) {
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
      <Button title="Volver" onPress={onBack} />
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
