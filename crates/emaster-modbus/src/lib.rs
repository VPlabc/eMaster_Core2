#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ModbusError {
    InvalidAddress,
    InvalidQuantity,
    InvalidResponse,
    Exception(u8),
    Transport,
}

pub trait ModbusTransport {
    fn transact(&mut self, request: &[u8]) -> Result<Vec<u8>, ModbusError>;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModbusClient<T> {
    pub transport: T,
    pub unit_id: u8,
}

impl<T: ModbusTransport> ModbusClient<T> {
    pub fn read_holding_registers(
        &mut self,
        start: u16,
        quantity: u16,
    ) -> Result<Vec<u16>, ModbusError> {
        if quantity == 0 || quantity > 125 {
            return Err(ModbusError::InvalidQuantity);
        }
        let request = [
            0,
            1,
            0,
            0,
            0,
            6,
            self.unit_id,
            3,
            (start >> 8) as u8,
            start as u8,
            (quantity >> 8) as u8,
            quantity as u8,
        ];
        decode_holding_registers(&self.transport.transact(&request)?, self.unit_id, quantity)
    }

    pub fn write_single_register(&mut self, address: u16, value: u16) -> Result<(), ModbusError> {
        let request = [
            0,
            2,
            0,
            0,
            0,
            6,
            self.unit_id,
            6,
            (address >> 8) as u8,
            address as u8,
            (value >> 8) as u8,
            value as u8,
        ];
        let response = self.transport.transact(&request)?;
        if response.len() != 12
            || response[6] != self.unit_id
            || response[7] != 6
            || response[8..12] != request[8..12]
        {
            return Err(ModbusError::InvalidResponse);
        }
        Ok(())
    }
}

pub fn decode_holding_registers(
    response: &[u8],
    unit_id: u8,
    quantity: u16,
) -> Result<Vec<u16>, ModbusError> {
    if response.len() < 9 || response[6] != unit_id {
        return Err(ModbusError::InvalidResponse);
    }
    if response[7] & 0x80 != 0 {
        return Err(ModbusError::Exception(response[8]));
    }
    if response[7] != 3 {
        return Err(ModbusError::InvalidResponse);
    }
    let expected = quantity as usize * 2;
    if response[8] as usize != expected || response.len() != 9 + expected {
        return Err(ModbusError::InvalidResponse);
    }
    Ok(response[9..]
        .chunks_exact(2)
        .map(|b| u16::from_be_bytes([b[0], b[1]]))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Mock(Vec<u8>);
    impl ModbusTransport for Mock {
        fn transact(&mut self, _: &[u8]) -> Result<Vec<u8>, ModbusError> {
            Ok(self.0.clone())
        }
    }
    #[test]
    fn reads_registers_and_rejects_bad_quantity() {
        let response = vec![0, 1, 0, 0, 0, 7, 1, 3, 4, 0x12, 0x34, 0xab, 0xcd];
        let mut client = ModbusClient {
            transport: Mock(response),
            unit_id: 1,
        };
        assert_eq!(
            client.read_holding_registers(0, 2).unwrap(),
            vec![0x1234, 0xabcd]
        );
        assert_eq!(
            client.read_holding_registers(0, 0),
            Err(ModbusError::InvalidQuantity)
        );
    }
    #[test]
    fn rejects_exception_and_malformed_length() {
        assert_eq!(
            decode_holding_registers(&[0, 1, 0, 0, 0, 3, 1, 0x83, 2], 1, 1),
            Err(ModbusError::Exception(2))
        );
        assert_eq!(
            decode_holding_registers(&[0, 1, 0, 0, 0, 5, 1, 3, 2, 1], 1, 2),
            Err(ModbusError::InvalidResponse)
        );
    }

    #[test]
    fn writes_single_register_and_checks_echo() {
        let mut client = ModbusClient {
            transport: Mock(vec![0, 2, 0, 0, 0, 6, 1, 6, 0, 4, 0xab, 0xcd]),
            unit_id: 1,
        };
        assert_eq!(client.write_single_register(4, 0xabcd), Ok(()));
    }
}
