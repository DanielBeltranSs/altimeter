import React, { useState, useContext } from 'react';
import { View, Text, Button, Alert, StyleSheet, ActivityIndicator } from 'react-native';
import * as DocumentPicker from 'expo-document-picker';
import * as FileSystem from 'expo-file-system';
import { Buffer } from 'buffer';
import { BleContext } from './BleProvider';

const DFU_SERVICE_UUID = "e3c0f200-3b0b-4253-9f53-3a351d8a146e";
const DFU_CHARACTERISTIC_UUID = "e3c0f200-3b0b-4253-9f53-3a351d8a146e";

export default function FirmwareUpdate({ onBack }) {
  const { connectedDevice } = useContext(BleContext);
  const [updating, setUpdating] = useState(false);
  const [progress, setProgress] = useState(0);

  const pickFirmwareFile = async () => {
    try {
      const result = await DocumentPicker.getDocumentAsync({
        type: "*/*",
        copyToCacheDirectory: true,
      });
      if (!result.canceled && result.assets && result.assets.length > 0) {
        return result.assets[0].uri;
      }
      return null;
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
    // Delay inicial para estabilizar la conexión
    await new Promise(resolve => setTimeout(resolve, 100));
    console.log("Enviando firmware al DFU Service:", DFU_SERVICE_UUID, "Característica:", DFU_CHARACTERISTIC_UUID);
    for (let i = 0; i < chunks.length; i++) {
      const chunk = chunks[i];
      const base64Chunk = chunk.toString('base64');
      console.log(`Enviando chunk ${i + 1} de ${chunks.length} al servicio DFU`);
      try {
        await device.writeCharacteristicWithoutResponseForService(
          DFU_SERVICE_UUID,
          DFU_CHARACTERISTIC_UUID,
          base64Chunk
        );
      } catch (error) {
        console.error(`Error al enviar chunk ${i + 1}:`, error);
        throw error;
      }
      setProgress(Math.round(((i + 1) / chunks.length) * 100));
      // Pausa breve para no saturar la conexión BLE
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    // Enviar el marcador de fin (EOF) en base64
    const eofMarker = Buffer.from("EOF").toString('base64');
    console.log("Enviando marcador EOF al servicio DFU");
    await device.writeCharacteristicWithoutResponseForService(
      DFU_SERVICE_UUID,
      DFU_CHARACTERISTIC_UUID,
      eofMarker
    );
  };

  const updateFirmware = async () => {
    if (!connectedDevice) {
      Alert.alert("Error", "No hay dispositivo conectado.");
      return;
    }

    try {
      setUpdating(true);
      setProgress(0);

      const fileUri = await pickFirmwareFile();
      if (!fileUri) {
        Alert.alert("Error", "No se seleccionó ningún archivo.");
        setUpdating(false);
        return;
      }

      const firmwareBuffer = await readFirmwareFile(fileUri);
      // Negociar el MTU (se solicita 247; el resultado puede ser mayor)
      const mtuResult = await connectedDevice.requestMTU(247);
      const negotiatedMtu = mtuResult?.mtu || 247;
      // Usamos un chunk size de 300 bytes (o menor si el MTU lo limita)
      const chunkSize = Math.min(300, negotiatedMtu - 3);
      console.log("MTU negociado:", negotiatedMtu, "Chunk size:", chunkSize);

      const chunks = chunkFirmware(firmwareBuffer, chunkSize);
      console.log("Total de chunks a enviar:", chunks.length);
      await sendFirmware(connectedDevice, chunks);

      Alert.alert("Éxito", "Firmware actualizado correctamente.");
    } catch (error) {
      console.error("Error en actualización DFU:", error);
      Alert.alert("Error", error.message);
    } finally {
      setUpdating(false);
    }
  };

  if (!connectedDevice) {
    return (
      <View style={styles.container}>
        <ActivityIndicator size="large" color="#007AFF" />
        <Text style={{ textAlign: 'center', marginTop: 10 }}>
          Esperando conexión con el dispositivo BLE...
        </Text>
      </View>
    );
  }

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
