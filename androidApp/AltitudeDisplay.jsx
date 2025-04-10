// AltitudeDisplay.jsx
import React, { useState, useEffect, useContext, useRef } from 'react';
import { View, Text, StyleSheet, ActivityIndicator } from 'react-native';
import { BleContext } from './BleProvider';
import { Buffer } from 'buffer';
import Tts from 'react-native-tts';

const SERVICE_UUID = "4fafc200-1fb5-459e-8fcc-c5c9c331914b";
const CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";

const AltitudeDisplay = () => {
  const { connectedDevice } = useContext(BleContext);
  const [altitude, setAltitude] = useState(null);
  
  // Usamos un ref para almacenar el último valor leído y evitar hablar de cambios muy pequeños
  const lastSpokenAltitude = useRef(null);

  // Configuración de la voz
  useEffect(() => {
    Tts.setDefaultLanguage('es-ES');
    Tts.setDefaultRate(0.5);
    Tts.setDefaultPitch(1.0);
  }, []);
  
  useEffect(() => {
    let subscription;
    if (connectedDevice) {
      console.log("Suscribiéndose a notificaciones de altitud...");
      subscription = connectedDevice.monitorCharacteristicForService(
        SERVICE_UUID,
        CHARACTERISTIC_UUID,
        (error, characteristic) => {
          if (error) {
            console.error("Error en notificación de altitud:", error);
            return;
          }
          if (characteristic && characteristic.value) {
            // Se asume que el firmware envía un string (por ejemplo, "12999") codificado en base64.
            const decoded = Buffer.from(characteristic.value, 'base64').toString('utf-8');
            setAltitude(decoded);
          }
        }
      );
    }
    return () => {
      if (subscription) {
        subscription.remove();
      }
    };
  }, [connectedDevice]);

  // Leer la altitud cuando cambie significativamente
  useEffect(() => {
    if (altitude !== null) {
      // Convertir el valor a número para comparación (opcional, según el formato)
      const currentValue = parseFloat(altitude);
      // Si no se ha hablado nunca o ha cambiado en más de 1 unidad, por ejemplo:
      if (lastSpokenAltitude.current === null || Math.abs(currentValue - lastSpokenAltitude.current) >= 1) {
        // Construir el mensaje. Podrías agregar "metros" u otra descripción.
        const message = `Altitud ${currentValue} metros`;
        console.log("Leyendo por voz:", message);
        Tts.speak(message);
        lastSpokenAltitude.current = currentValue;
      }
    }
  }, [altitude]);

  return (
    <View style={styles.container}>
      {connectedDevice ? (
        altitude !== null ? (
          <Text style={styles.altitudeText}>Altitud: {altitude} m</Text>
        ) : (
          <ActivityIndicator size="large" color="#007AFF" />
        )
      ) : (
        <Text style={styles.infoText}>No conectado</Text>
      )}
    </View>
  );
};

const styles = StyleSheet.create({
  container: {
    marginTop: 20,
    alignItems: 'center'
  },
  altitudeText: {
    fontSize: 26,
    fontWeight: 'bold',
    color: '#007AFF'
  },
  infoText: {
    fontSize: 18,
    color: '#444'
  }
});

export default AltitudeDisplay;
