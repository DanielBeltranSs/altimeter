// AltitudeDisplay.jsx
import React, { useState, useEffect, useContext, useRef } from 'react';
import { View, Text, StyleSheet, ActivityIndicator, TextInput, Button, ScrollView } from 'react-native';
import { BleContext } from './BleProvider';
import { Buffer } from 'buffer';
import Tts from 'react-native-tts';

const SERVICE_UUID = "4fafc200-1fb5-459e-8fcc-c5c9c331914b";
const CHARACTERISTIC_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";

const AltitudeDisplay = () => {
  const { connectedDevice } = useContext(BleContext);
  const [altitude, setAltitude] = useState(null);
  
  // Guarda la última altitud en la que se habló para evitar mensajes muy repetitivos.
  const lastSpokenAltitude = useRef(null);

  // Estado para el input del usuario y el listado de alertas
  const [inputAltitude, setInputAltitude] = useState('');
  const [alertAltitudes, setAlertAltitudes] = useState([]);
  
  // Ref para recordar qué alertas ya se han activado en el rango actual
  const triggeredAlertsRef = useRef({});

  // Configuración de la voz
  useEffect(() => {
    Tts.setDefaultLanguage('es-ES');
    Tts.setDefaultRate(0.5);
    Tts.setDefaultPitch(1.0);
  }, []);
  
  // Monitor de la altitud vía BLE
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

  // Efecto para hablar la altitud de forma general (cuando cambie significativamente)
  /*useEffect(() => {
    if (altitude !== null) {
      const currentValue = parseFloat(altitude);
      // Si nunca se ha hablado o la diferencia es mayor o igual a 1 unidad:
      if (lastSpokenAltitude.current === null || Math.abs(currentValue - lastSpokenAltitude.current) >= 1) {
        const message = `Altitud ${currentValue} pies`;
        console.log("Leyendo por voz:", message);
        Tts.speak(message);
        lastSpokenAltitude.current = currentValue;
      }
    }
  }, [altitude]);*/

  // Efecto para verificar alertas programadas cuando cambia la altitud
  useEffect(() => {
    if (altitude !== null) {
      const currentValue = parseFloat(altitude);
      const tolerance = 1; // tolerancia de 1 metro
      alertAltitudes.forEach(alertValue => {
        const diff = Math.abs(currentValue - alertValue);
        if (diff < tolerance) {
          // Si la alerta no se ha activado aún en este rango, se activa
          if (!triggeredAlertsRef.current[alertValue]) {
            const alertMessage = `altitud de ${alertValue} pies`;
            console.log("Alerta por voz:", alertMessage);
            Tts.speak(alertMessage);
            triggeredAlertsRef.current[alertValue] = true;
          }
        } else {
          // Si se sale de la zona de tolerancia, se resetea para futuras activaciones
          if (triggeredAlertsRef.current[alertValue]) {
            triggeredAlertsRef.current[alertValue] = false;
          }
        }
      });
    }
  }, [altitude, alertAltitudes]);

  // Función para agregar una alerta a la lista
  const handleAddAlert = () => {
    const numericValue = parseFloat(inputAltitude);
    if (!isNaN(numericValue)) {
      // Evitar duplicados
      if (!alertAltitudes.includes(numericValue)) {
        setAlertAltitudes([...alertAltitudes, numericValue]);
        setInputAltitude('');
      } else {
        console.log("La alerta para esa altitud ya existe");
      }
    } else {
      console.log("Por favor ingrese un número válido");
    }
  };

  return (
    <ScrollView contentContainerStyle={styles.scrollContainer}>
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

        {/* Sección para agregar alertas */}
        <View style={styles.alertSection}>
          <Text style={styles.infoText}>Agregar alerta para altitud:</Text>
          <TextInput
            style={styles.input}
            value={inputAltitude}
            onChangeText={setInputAltitude}
            placeholder="Ej. 150"
            keyboardType="numeric"
          />
          <Button title="Añadir alerta" onPress={handleAddAlert} />
        </View>

        {/* Mostrar las alertas agregadas */}
        {alertAltitudes.length > 0 && (
          <View style={styles.alertList}>
            <Text style={styles.infoText}>Alertas activas:</Text>
            {alertAltitudes.map((alertValue, index) => (
              <Text key={index} style={styles.alertItem}>{alertValue} m</Text>
            ))}
          </View>
        )}
      </View>
    </ScrollView>
  );
};

const styles = StyleSheet.create({
  scrollContainer: {
    flexGrow: 1,
    padding: 20,
    alignItems: 'center'
  },
  container: {
    alignItems: 'center',
    width: '100%'
  },
  altitudeText: {
    fontSize: 26,
    fontWeight: 'bold',
    color: '#007AFF',
    marginBottom: 20
  },
  infoText: {
    fontSize: 18,
    color: '#444',
    marginBottom: 10
  },
  alertSection: {
    width: '100%',
    marginVertical: 20,
    alignItems: 'center'
  },
  input: {
    width: '80%',
    borderColor: '#ccc',
    borderWidth: 1,
    borderRadius: 4,
    paddingHorizontal: 10,
    paddingVertical: 5,
    marginBottom: 10,
    fontSize: 16
  },
  alertList: {
    width: '100%',
    alignItems: 'center'
  },
  alertItem: {
    fontSize: 16,
    color: '#333'
  }
});

export default AltitudeDisplay;
