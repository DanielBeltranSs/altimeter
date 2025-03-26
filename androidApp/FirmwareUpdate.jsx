import React, { useState } from 'react';
import { View, Text, Button, Alert, StyleSheet, ActivityIndicator } from 'react-native';
import * as DocumentPicker from 'expo-document-picker';
import * as FileSystem from 'expo-file-system';
import { Buffer } from 'buffer';

const DFU_SERVICE_UUID = "e3c0f200-3b0b-4253-9f53-3a351d8a146e";
const DFU_CHARACTERISTIC_UUID = "e3c0f200-3b0b-4253-9f53-3a351d8a146e";

export default function FirmwareUpdate({ onBack, connectedDevice }) {
  const [updating, setUpdating] = useState(false);
  const [progress, setProgress] = useState(0);

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

  const readFirmwareFile = async (uri) => {
    const base64 = await FileSystem.readAsStringAsync(uri, { encoding: FileSystem.EncodingType.Base64 });
    return Buffer.from(base64, 'base64');
  };

  const chunkFirmware = (buffer, chunkSize) => {
    const chunks = [];
    for (let i = 0; i < buffer.length; i += chunkSize) {
      chunks.push(buffer.slice(i, i + chunkSize));
    }
    return chunks;
  };

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

    await device.writeCharacteristicWithResponseForService(
      DFU_SERVICE_UUID,
      DFU_CHARACTERISTIC_UUID,
      "EOF"
    );
  };

  const updateFirmware = async () => {
    try {
      if (!connectedDevice) {
        Alert.alert("Error", "Ningún dispositivo conectado.");
        return;
      }

      setUpdating(true);
      setProgress(0);

      const fileUri = await pickFirmwareFile();
      if (!fileUri) {
        Alert.alert("Error", "No se seleccionó archivo.");
        setUpdating(false);
        return;
      }

      const firmwareBuffer = await readFirmwareFile(fileUri);
      const mtuResult = await connectedDevice.requestMTU(247);
      const negotiatedMtu = mtuResult.mtu ? mtuResult.mtu : 247;
      const chunkSize = Math.min(200, negotiatedMtu - 3);

      const chunks = chunkFirmware(firmwareBuffer, chunkSize);
      await sendFirmware(connectedDevice, chunks);

      Alert.alert("Éxito", "Firmware actualizado correctamente.");
      setUpdating(false);
    } catch (error) {
      console.error("Error en actualización DFU:", error);
      Alert.alert("Error", error.message);
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
  container: { flex: 1, padding: 20, justifyContent: 'center', backgroundColor: '#fff' },
  title: { fontSize: 22, textAlign: 'center', marginBottom: 20 },
  progressContainer: { alignItems: 'center' },
  progressText: { marginTop: 10, fontSize: 18 },
});
