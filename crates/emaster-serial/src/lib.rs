#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SerialError {
    InvalidConfiguration,
    NotOpen,
    Io,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SerialConfig {
    pub path: String,
    pub baud_rate: u32,
    pub timeout_ms: u64,
}

pub trait SerialPort {
    fn open(&mut self, config: SerialConfig) -> Result<(), SerialError>;
    fn close(&mut self);
    fn read(&mut self, output: &mut [u8]) -> Result<usize, SerialError>;
    fn write(&mut self, input: &[u8]) -> Result<usize, SerialError>;
    fn is_open(&self) -> bool;
}

#[derive(Debug, Default)]
pub struct MemorySerialPort {
    open: bool,
    rx: Vec<u8>,
    tx: Vec<u8>,
}

impl MemorySerialPort {
    pub fn feed(&mut self, data: &[u8]) {
        self.rx.extend_from_slice(data);
    }
    pub fn transmitted(&self) -> &[u8] {
        &self.tx
    }
}

impl SerialPort for MemorySerialPort {
    fn open(&mut self, config: SerialConfig) -> Result<(), SerialError> {
        validate_config(&config)?;
        self.open = true;
        Ok(())
    }
    fn close(&mut self) {
        self.open = false;
    }
    fn read(&mut self, output: &mut [u8]) -> Result<usize, SerialError> {
        if !self.open {
            return Err(SerialError::NotOpen);
        }
        let count = output.len().min(self.rx.len());
        output[..count].copy_from_slice(&self.rx[..count]);
        self.rx.drain(..count);
        Ok(count)
    }
    fn write(&mut self, input: &[u8]) -> Result<usize, SerialError> {
        if !self.open {
            return Err(SerialError::NotOpen);
        }
        self.tx.extend_from_slice(input);
        Ok(input.len())
    }
    fn is_open(&self) -> bool {
        self.open
    }
}

pub fn validate_config(config: &SerialConfig) -> Result<(), SerialError> {
    if config.path.trim().is_empty() || config.baud_rate == 0 || config.timeout_ms == 0 {
        return Err(SerialError::InvalidConfiguration);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validates_two_independent_configurations() {
        let a = SerialConfig {
            path: "COM1".into(),
            baud_rate: 115200,
            timeout_ms: 1000,
        };
        let b = SerialConfig {
            path: "/dev/ttyUSB1".into(),
            baud_rate: 9600,
            timeout_ms: 500,
        };
        assert!(validate_config(&a).is_ok());
        assert!(validate_config(&b).is_ok());
        assert_eq!(
            validate_config(&SerialConfig {
                path: "".into(),
                baud_rate: 1,
                timeout_ms: 1
            }),
            Err(SerialError::InvalidConfiguration)
        );
    }

    #[test]
    fn memory_port_has_explicit_open_close_and_io_lifecycle() {
        let mut port = MemorySerialPort::default();
        assert_eq!(port.write(b"x"), Err(SerialError::NotOpen));
        port.open(SerialConfig {
            path: "COM1".into(),
            baud_rate: 115200,
            timeout_ms: 10,
        })
        .unwrap();
        port.feed(b"rx");
        let mut out = [0; 2];
        assert_eq!(port.read(&mut out), Ok(2));
        assert_eq!(&out, b"rx");
        assert_eq!(port.write(b"tx"), Ok(2));
        port.close();
        assert!(!port.is_open());
    }
}
