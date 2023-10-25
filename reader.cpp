#include "reader.hpp"
#include <bitset>

#include <QException>
#include <QDebug>


using namespace std;


Reader::Reader(ReaderParameters params) {

	// set values
	params.decim = false; // no oversampling
	params.port = 23456;	

	// temporary buffer to read supernumerous packets
	temp_buffer = new char[temp_buffer_size];

	// set up configuration byte string
	QByteArray configuration(40, 0);

	// ACQ_SETT byte
	bitset<8> acq_sett;
	acq_sett[7] = 1; // fixed
	acq_sett[6] = params.decim; // DECIM; 0: sample with fs, 1: sample with 10240 & downsample
	acq_sett[5] = 0; // REC_ON
	std::bitset<8> fsamp = binaryEncodingOfIndex<uint16_t>(params.fs, std::vector<uint16_t> {512, 2048, 5120, 10240});
	acq_sett |= fsamp << 3; // FSAMP, bit 3:4
	acq_sett |= bitset<8>(params.active_inputs - 1) << 1; // NCH, bit 1:2
	acq_sett[0] = 1; // ACQ_ON
	configuration[0] = acq_sett.to_ulong();

	// AN_OUT_IN_SEL byte
	bitset<8> an_out_in_sel;
	std::bitset<8> anout_gain = binaryEncodingOfIndex<uint16_t>(params.output_gain, std::vector<uint16_t> {1, 2, 4, 16});
	an_out_in_sel |= anout_gain << 4; // ANOUT_GAIN, bit 4:5
	std::bitset<8> insel = binaryEncodingOfIndex<Connector>(params.source_connector, std::vector<Connector> {
		Connector::in1, Connector::in2, Connector::in3, Connector::in4, Connector::in5, Connector::in6, Connector::in7, Connector::in8,
			Connector::multin1, Connector::multin2, Connector::multin3, Connector::multin4,
			Connector::auxin});
	an_out_in_sel |= insel; // INSEL, bit 0:3
	configuration[1] = an_out_in_sel.to_ulong();

	// AN_OUT_CH_SEL byte
	bitset<8> an_out_ch_sel;
	an_out_ch_sel = bitset<8>(params.source_channel - 1);
	configuration[2] = an_out_ch_sel.to_ulong();

	// INx_CONF bytes
	for (int in_idx = 0; in_idx < 8; in_idx++) {
		// INx_CONF0
		bitset<8> inx_conf0;
		configuration[3 + 3 * in_idx] = inx_conf0.to_ulong();

		// INx_CONF1
		bitset<8> inx_conf1;
		configuration[3 + 3 * in_idx + 1] = inx_conf1.to_ulong();

		// INx_CONF2
		bitset<8> inx_conf2;	
		std::bitset<8> hpf = binaryEncodingOfIndex<uint16_t>(params.in_hp[in_idx], std::vector<uint16_t> {0, 10, 100, 200});
		inx_conf2 |= hpf << 4; // HPF, bit 4:5
		std::bitset<8> lpf = binaryEncodingOfIndex<uint16_t>(params.in_lp[in_idx], std::vector<uint16_t> {130, 500, 900, 4400});
		inx_conf2 |= lpf << 2; // LPF, bit 2:3		
		std::bitset<8> mode = binaryEncodingOfIndex<InputMode>(params.in_mode[in_idx], std::vector<InputMode> {InputMode::monopolar, InputMode::differential, InputMode::bipolar});
		inx_conf2 |= mode; // MODE, bit 0:1
		configuration[3 + 3 * in_idx + 2] = inx_conf2.to_ulong();
	}

	// MULTIPLE_INx_CONF bytes
	for (int multin_idx = 0; multin_idx < 4; multin_idx++) {
		// MULTIPLE_INx_CONF0
		bitset<8> multiple_inx_conf0;
		configuration[27 + 3 * multin_idx] = multiple_inx_conf0.to_ulong();

		// MULTIPLE_INx_CONF1
		bitset<8> multiple_inx_conf1;
		configuration[27 + 3 * multin_idx + 1] = multiple_inx_conf1.to_ulong();

		// MULTIPLE_INx_CONF2
		bitset<8> multiple_inx_conf2;
		std::bitset<8> hpf = binaryEncodingOfIndex<uint16_t>(params.multin_hp[multin_idx], std::vector<uint16_t> {0, 10, 100, 200});
		multiple_inx_conf2 |= hpf << 4; // HPF, bit 4:5
		std::bitset<8> lpf = binaryEncodingOfIndex<uint16_t>(params.multin_lp[multin_idx], std::vector<uint16_t> {130, 500, 900, 4400});
		multiple_inx_conf2 |= lpf << 2; // LPF, bit 2:3		
		std::bitset<8> mode = binaryEncodingOfIndex<InputMode>(params.multin_mode[multin_idx], std::vector<InputMode> {InputMode::monopolar, InputMode::differential, InputMode::bipolar});
		multiple_inx_conf2 |= mode; // MODE, bit 0:1
		configuration[27 + 3 * multin_idx + 2] = multiple_inx_conf2.to_ulong();
	}

	// CRC
	bitset<8> crc_byte;
	crc_byte = crc(configuration);
	configuration[39] = crc_byte.to_ulong();


	// open socket
	socket = new QTcpSocket();
	socket->connectToHost(params.ip_address, params.port);
	if (socket->waitForConnected(connection_timeout) == false) {
		error_msg = "error when connecting to amplifier: " + socket->errorString();
		return;
	}

	// send configuration with trigger low
	//printConfiguration(configuration);
	socket->write(configuration);
	socket->flush();

	// send configuration with trigger high (this code causes troubles and is therefore deactivated)
	//acq_sett[5] = 1; // REC_ON
	//configuration[0] = acq_sett.to_ulong();
	//crc_byte = crc(configuration);
	//configuration[39] = crc_byte.to_ulong();
	//socket->write(configuration);
	//socket->flush();
}	


Reader::~Reader() noexcept {
	// stop recording
	QByteArray configuration(40, 0);
	bitset<8> acq_sett;
	acq_sett[7] = 1; // fixed
	configuration[0] = acq_sett.to_ulong();
	bitset<8> crc_byte;
	crc_byte = crc(configuration);
	configuration[39] = crc_byte.to_ulong();
	socket->write(configuration);
	socket->flush();

	// close & delete socket
	socket->close();
	delete socket;

	delete temp_buffer;
}


bool Reader::getData(std::vector<int16_t> &buffer) {
	static bool ret_val;
	static uint32_t read_bytes;
	static uint32_t supernumerous_bytes;

	if (socket->state() == QTcpSocket::SocketState::ConnectedState) {
		for (;;) {
			// wait for new data packets
			ret_val = socket->waitForReadyRead(10000); // 10s timeout
			if (!ret_val) {
				error_msg = socket->errorString();
				return false;
			}

			// DEPRECATED CODE: check if there are supernumerous packets in the buffer, if so, read & omit them (otherwise they would get a wrong timestamp)
			/*
			supernumerous_bytes = 0;
			while (socket->bytesAvailable() >= static_cast<qint64>(2 * sizeof(buffer[0]) * buffer.size())) {
				read_bytes = socket->read(temp_buffer, sizeof(buffer[0]) * buffer.size());
				if (read_bytes != sizeof(buffer[0]) * buffer.size()) {
					error_msg = "mismatch between expected and received number of bytes";
					return false;
				}
				supernumerous_bytes += read_bytes;
			}
			if (supernumerous_bytes > 0) {
				qDebug() << "omitting" << read_bytes << "bytes in network buffer";
			}
			*/

			// new behaviour: never omit any samples because inaccurate timestamps can be corrected
			if (socket->bytesAvailable() >= static_cast<qint64>(2 * sizeof(buffer[0]) * buffer.size())) {
				qDebug() << "application could not read all network data in time";
			}

			// check if enough data are available to read a full block
			if (socket->bytesAvailable() >= static_cast<qint64>(sizeof(buffer[0]) * buffer.size())) {
				// read data from amplifier
				read_bytes = socket->read(reinterpret_cast<char*>(buffer.data()), sizeof(buffer[0]) * buffer.size());
				if (read_bytes != sizeof(buffer[0]) * buffer.size()) {
					error_msg = "mismatch between expected and received number of bytes";
					return false;
				}
				return true;
			}
		}
	}
	else {
		error_msg = "network socket is not available";
		return false;
	}
}


// find the index of "item" in "sorted_list" and convert the index into a bit string
template<class T>
std::bitset<8> Reader::binaryEncodingOfIndex(T item, std::vector<T> sorted_list) const
{
	std::vector<T>::iterator itr = std::find(sorted_list.begin(), sorted_list.end(), item);
	if (itr != sorted_list.end()) {
		uint16_t item_idx = std::distance(sorted_list.begin(), itr);
		return bitset<8>(item_idx);
	}
	else {
		qDebug() << "element not found";
		throw QException();
	}
}


// CRC function used by OTB
std::bitset<8> Reader::crc(const QByteArray& configuration) const
{
	bool xor (false);
	bitset<8> crc;
	bitset<8> conf_byte;
	bitset<8> byte_140(140);
	for (int byte_idx = 0; byte_idx < 39; byte_idx++) {
		conf_byte = std::bitset<8>(configuration[byte_idx]);
		for (int bit_idx = 7; bit_idx >= 0; bit_idx--) {
			xor = crc[0] != conf_byte[0];
			crc = crc >> 1;
			if (xor)
				crc ^= byte_140;
			conf_byte = conf_byte >> 1;
		}
	}

	return crc;
}


// pretty-print configuration byte string (for debugging)
void Reader::printConfiguration(const QByteArray& configuration) const
{
	for (int byte_idx = 0; byte_idx <= configuration.size(); byte_idx++)
		qInfo("%2d\t%s", byte_idx, bitset<8>(configuration[byte_idx]).to_string().c_str());
}
