#include "mainwindow.hpp"
#include "ui_mainwindow.h"

#include <fstream>
#include <cmath>

#include <QCloseEvent>
#include <QDateTime>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QException>
#include <QDebug>

#include <lsl_cpp.h>

using namespace std;


// recording thread
void recording_thread_function(
	MainWindow* win, std::string name, ReaderParameters params, std::atomic<bool>& shutdown) {

	// determine channel settings based on activated input blocks (i.e. select number of active input blocks)
	uint64_t samples_per_block;
	uint64_t n_amp_channels;
	uint64_t multin_channel_offset;
	uint64_t aux_channel_offset;
	uint64_t accessory_channel_offset;
	if (params.in_active[7] || params.in_active[6] || params.multin_active[3]) {
		params.active_inputs = 4; // amplifier records and sends 408 channels
		n_amp_channels = 408;
		samples_per_block = 1632 / 2 / n_amp_channels; // determined by amplifier based on "active_inputs"; account for 2 bytes per channel value (int16); --> 2
		multin_channel_offset = 128;
		aux_channel_offset = n_amp_channels - 24;
		accessory_channel_offset = n_amp_channels - 8;
	} else if (params.in_active[5] || params.in_active[4] || params.multin_active[2]) {
		params.active_inputs = 3; // amplifier records and sends 312 channels
		n_amp_channels = 312;
		samples_per_block = 2496 / 2 / n_amp_channels; // --> 4
		multin_channel_offset = 96;
		aux_channel_offset = n_amp_channels - 24;
		accessory_channel_offset = n_amp_channels - 8;
	}
	else if (params.in_active[3] || params.in_active[2] || params.multin_active[1]) {
		params.active_inputs = 2; // amplifier records and sends 216 channels
		n_amp_channels = 216;
		samples_per_block = 2592 / 2 / n_amp_channels; // --> 6
		multin_channel_offset = 64;
		aux_channel_offset = n_amp_channels - 24;
		accessory_channel_offset = n_amp_channels - 8;
	}
	else if (params.in_active[1] || params.in_active[0] || params.multin_active[0] || params.aux_active) {
		params.active_inputs = 1; // amplifier records and sends 120 channels
		n_amp_channels = 120;
		samples_per_block = 2880 / 2 / n_amp_channels; // --> 12
		multin_channel_offset = 32;
		aux_channel_offset = n_amp_channels - 24;
		accessory_channel_offset = n_amp_channels - 8;
	}
	else {
		win->errorWhenRecording("no input block selected");
		return;
	}
	qDebug() << "set \"active input\" to " << params.active_inputs;

	// determine indices of channels which are going to be send via LSL
	const uint16_t number_of_IN_blocks = 8;
	const uint16_t number_of_MULTIN_blocks = 4;
	const uint16_t channels_per_IN_block = 16;
	const uint16_t channels_per_MULTIN_block = 64;
	const uint16_t channels_per_AUX_block = 16;
	const double in_gain = 5.0 / pow(2.0, 16) / 150.0 * 1.0e6; // used to convert integers in IN/MULTIN blocks to microV; note: 150 comprises of a preamplifcation factor of 5 and an amplifcation factor of 30
	const double aux_gain = 5.0 / pow(2.0, 16) / 0.5 * 1.0e6; // used to convert integers in AUX block to microV

	std::vector<int16_t> selected_amp_channel_idcs;
	std::vector<double> channel_gain;
	std::vector<string> channel_labels;
	// add 16-channels blocks (INx)
	for (int block_idx = 0; block_idx < number_of_IN_blocks; block_idx++) {
		if (params.in_active[block_idx]) {
			for (int channel_idx = 0; channel_idx < channels_per_IN_block; channel_idx++) {
				selected_amp_channel_idcs.push_back(block_idx * channels_per_IN_block + channel_idx);
				channel_gain.push_back(in_gain);
				channel_labels.push_back(string("IN") + std::to_string(block_idx + 1) + string("-") + std::to_string(channel_idx + 1));
			}
		}
	}
	// add 64-channels blocks (MULTINx)
	for (int block_idx = 0; block_idx < number_of_MULTIN_blocks; block_idx++) {
		if (params.multin_active[block_idx]) {
			for (int channel_idx = 0; channel_idx < channels_per_MULTIN_block; channel_idx++) {
				selected_amp_channel_idcs.push_back(multin_channel_offset + block_idx * static_cast<uint64_t>(channels_per_MULTIN_block) + channel_idx);
				channel_gain.push_back(in_gain);
				channel_labels.push_back(string("MULTIN") + std::to_string(block_idx + 1) + string("-") + std::to_string(channel_idx + 1));
			}
		}
	}
	// add AUX channels (one block)
	if (params.aux_active) {
		for (int channel_idx = 0; channel_idx < channels_per_AUX_block; channel_idx++) {
			selected_amp_channel_idcs.push_back(aux_channel_offset + channel_idx);
			channel_gain.push_back(aux_gain);
			channel_labels.push_back(string("AUX") + string("-") + std::to_string(channel_idx + 1));
		}
	}
	// add accessory channels (ATTENTION: accessory channels have integer-values but are streamed as float-values via LSL)
	selected_amp_channel_idcs.push_back(accessory_channel_offset + 0);
	channel_gain.push_back(1);
	channel_labels.push_back("SampleCounter");
	selected_amp_channel_idcs.push_back(accessory_channel_offset + 1);
	channel_gain.push_back(1);
	channel_labels.push_back("Trigger");

	uint64_t n_lsl_channels = selected_amp_channel_idcs.size();

	// create LSL stream info and declare channel labels
	lsl::stream_info info(name, "ExG", n_lsl_channels, params.fs, lsl::cf_float32, "quattrocento_id42");
	lsl::xml_element chns = info.desc().append_child("channels");
	for (const string& label : channel_labels)
		chns.append_child("channel")
		.append_child_value("label", label)
		.append_child_value("unit", "microvolt");
	info.desc().append_child_value("manufacturer", "OT Bioelettronica");

	// create LSL stream outlet
	lsl::stream_outlet outlet(info);

	// set buffer size to the block size used by the amplifier; this way, the "getData" function returns as fast as possible
	std::vector<int16_t> amp_buffer(samples_per_block * n_amp_channels);

	// buffer for the final data to be send via LSL
	std::vector<float> lsl_buffer(n_lsl_channels*samples_per_block);

	// setup amplifier (i.e. create configuration byte-string and send it to the amplifier)
	Reader device(params);

	// acquire data, copy it to the buffer, select channels to send via LSL, and push it to the oulet
	uint64_t sample_idx;
	uint64_t lsl_channel_idx;
	while (!shutdown) {
		if (device.getData(amp_buffer)) {
			// copy selected channels from amp buffer into LSL buffer and multiply with gain factor
			for (sample_idx = 0; sample_idx < samples_per_block; sample_idx++)
				for (lsl_channel_idx = 0; lsl_channel_idx < n_lsl_channels; lsl_channel_idx++)
					lsl_buffer[n_lsl_channels * sample_idx + lsl_channel_idx] = amp_buffer[n_amp_channels * sample_idx + selected_amp_channel_idcs[lsl_channel_idx]] * channel_gain[lsl_channel_idx];

			outlet.push_chunk_multiplexed(lsl_buffer);
		}
		else {
			// there was a problem with the acquisition, display an error message and end the thread
			win->errorWhenRecording(device.error_msg);
			break;
		}
	}
}


// the constructor mainly sets up the `Ui::MainWindow` class and creates the connections between signals (e.g. 'button X was clicked') and slots
MainWindow::MainWindow(QWidget *parent, const char *config_file)
	: QMainWindow(parent), ui(new Ui::MainWindow) {
	ui->setupUi(this);

	// setup menu connections
	connect(ui->actionLoad_Configuration, &QAction::triggered, [this]() {
		load_config(QFileDialog::getOpenFileName(
			this, "Load Configuration File", "", "Configuration Files (*.cfg)"));
	});
	connect(ui->actionSave_Configuration, &QAction::triggered, [this]() {
		save_config(QFileDialog::getSaveFileName(
			this, "Save Configuration File", "", "Configuration Files (*.cfg)"));
	});
	connect(ui->actionQuit, &QAction::triggered, this, &MainWindow::close);
	connect(ui->actionAbout, &QAction::triggered, [this]() {
		QString infostr = QStringLiteral("LSL app for OTB Quattrocento amplifier") +
			QStringLiteral("\nAuthor: Patrick Ofner") +
			QStringLiteral("\npatrick.ofner@bcf.uni-freiburg.de") +
			QStringLiteral("\nhttps://www.brain.uni-freiburg.de") +
			QStringLiteral("\nLSL library version: ") + QString::number(lsl::library_version());
		QMessageBox::about(this, "LSL app for OTB Quattrocento ampflier", infostr);
	});
	connect(ui->linkButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);

	// setup reader connection
	connect(this, &MainWindow::errorWhenRecording, this, &MainWindow::showRecordingError);

	// load confiuration
	QString cfgfilepath = find_config_file(config_file);
	load_config(cfgfilepath);

	// setup up interface connections
	connect(ui->in1_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->in2_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->in3_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->in4_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->in5_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->in6_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->in7_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->in8_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->multin1_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->multin2_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->multin3_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->multin4_active, &QCheckBox::stateChanged, this, &MainWindow::updateInputConfiguration);
	connect(ui->sampling_frequency, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::updateLowHighPass);
	connect(ui->source_connector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MainWindow::updateSourceChannel);	
}


// load configuration
void MainWindow::load_config(const QString &filename) {
	QSettings settings(filename, QSettings::Format::IniFormat);

	// amplifier configuration
	ui->stream_name->setText(settings.value("stream_name", "quattrocento").toString()); // default LSL stream name
	ui->ip->setText(settings.value("ip", "169.254.1.10").toString()); // default IP of amplifier
	ui->sampling_frequency->setCurrentIndex(settings.value("sampling_frequency", 1).toInt()); // default: 2048 Hz

	// input configuration
	ui->in1_active->setChecked(settings.value("in1_active", 0).toBool()); // default: not active
	ui->in2_active->setChecked(settings.value("in2_active", 0).toBool());
	ui->in3_active->setChecked(settings.value("in3_active", 0).toBool());
	ui->in4_active->setChecked(settings.value("in4_active", 0).toBool());
	ui->in5_active->setChecked(settings.value("in5_active", 0).toBool());
	ui->in6_active->setChecked(settings.value("in6_active", 0).toBool());
	ui->in7_active->setChecked(settings.value("in7_active", 0).toBool());
	ui->in8_active->setChecked(settings.value("in8_active", 0).toBool());
	ui->multin1_active->setChecked(settings.value("multin1_active", 1).toBool()); // activated by default
	ui->multin2_active->setChecked(settings.value("multin2_active", 0).toBool());
	ui->multin3_active->setChecked(settings.value("multin3_active", 0).toBool());
	ui->multin4_active->setChecked(settings.value("multin4_active", 0).toBool());
	ui->aux_active->setChecked(settings.value("aux_active", 0).toBool());
	updateInputConfiguration();

	updateLowHighPass();
	ui->in1_mode->setCurrentIndex(settings.value("in1_mode", 0).toInt()); // default: monopolar
	ui->in1_hp->setCurrentIndex(settings.value("in1_hp", 0).toInt()); // default: 0.7 Hz
	ui->in1_lp->setCurrentIndex(settings.value("in1_lp", 2).toInt()); //default: 900 Hz
	ui->in2_mode->setCurrentIndex(settings.value("in2_mode", 0).toInt());
	ui->in2_hp->setCurrentIndex(settings.value("in2_hp", 0).toInt());
	ui->in2_lp->setCurrentIndex(settings.value("in2_lp", 2).toInt());
	ui->in3_mode->setCurrentIndex(settings.value("in3_mode", 0).toInt());
	ui->in3_hp->setCurrentIndex(settings.value("in3_hp", 0).toInt());
	ui->in3_lp->setCurrentIndex(settings.value("in3_lp", 2).toInt());
	ui->in4_mode->setCurrentIndex(settings.value("in4_mode", 0).toInt());
	ui->in4_hp->setCurrentIndex(settings.value("in4_hp", 0).toInt());
	ui->in4_lp->setCurrentIndex(settings.value("in4_lp", 2).toInt());
	ui->in5_mode->setCurrentIndex(settings.value("in5_mode", 0).toInt());
	ui->in5_hp->setCurrentIndex(settings.value("in5_hp", 0).toInt());
	ui->in5_lp->setCurrentIndex(settings.value("in5_lp", 2).toInt());
	ui->in6_mode->setCurrentIndex(settings.value("in6_mode", 0).toInt());
	ui->in6_hp->setCurrentIndex(settings.value("in6_hp", 0).toInt());
	ui->in6_lp->setCurrentIndex(settings.value("in6_lp", 2).toInt());
	ui->in7_mode->setCurrentIndex(settings.value("in7_mode", 0).toInt());
	ui->in7_hp->setCurrentIndex(settings.value("in7_hp", 0).toInt());
	ui->in7_lp->setCurrentIndex(settings.value("in7_lp", 2).toInt());
	ui->in8_mode->setCurrentIndex(settings.value("in8_mode", 0).toInt());
	ui->in8_hp->setCurrentIndex(settings.value("in8_hp", 0).toInt());
	ui->in8_lp->setCurrentIndex(settings.value("in8_lp", 2).toInt());
	ui->multin1_mode->setCurrentIndex(settings.value("multin1_mode", 0).toInt());
	ui->multin1_hp->setCurrentIndex(settings.value("multin1_hp", 0).toInt());
	ui->multin1_lp->setCurrentIndex(settings.value("multin1_lp", 2).toInt());
	ui->multin2_mode->setCurrentIndex(settings.value("multin2_mode", 0).toInt());
	ui->multin2_hp->setCurrentIndex(settings.value("multin2_hp", 0).toInt());
	ui->multin2_lp->setCurrentIndex(settings.value("multin2_lp", 2).toInt());
	ui->multin3_mode->setCurrentIndex(settings.value("multin3_mode", 0).toInt());
	ui->multin3_hp->setCurrentIndex(settings.value("multin3_hp", 0).toInt());
	ui->multin3_lp->setCurrentIndex(settings.value("multin3_lp", 2).toInt());
	ui->multin4_mode->setCurrentIndex(settings.value("multin4_mode", 0).toInt());
	ui->multin4_hp->setCurrentIndex(settings.value("multin4_hp", 0).toInt());
	ui->multin4_lp->setCurrentIndex(settings.value("multin4_lp", 2).toInt());

	// analog output configuration
	ui->source_connector->setCurrentIndex(settings.value("source_connector", 0).toInt()); // default: IN1
	updateSourceChannel();
	ui->source_channel->setCurrentIndex(settings.value("source_channel", 0).toInt()); // default: channel 1
	ui->output_gain->setCurrentIndex(settings.value("output_gain", 0).toInt()); // default: gain 1
}


// save function, same as above
void MainWindow::save_config(const QString &filename) {
	QSettings settings(filename, QSettings::Format::IniFormat);

	// amplifier configuration
	settings.setValue("stream_name", ui->stream_name->text());
	settings.setValue("ip", ui->ip->text());
	settings.setValue("sampling_frequency", ui->sampling_frequency->currentIndex());

	// input configuration
	settings.setValue("in1_active", ui->in1_active->isChecked());
	settings.setValue("in2_active", ui->in2_active->isChecked());
	settings.setValue("in3_active", ui->in3_active->isChecked());
	settings.setValue("in4_active", ui->in4_active->isChecked());
	settings.setValue("in5_active", ui->in5_active->isChecked());
	settings.setValue("in6_active", ui->in6_active->isChecked());
	settings.setValue("in7_active", ui->in7_active->isChecked());
	settings.setValue("in8_active", ui->in8_active->isChecked());
	settings.setValue("multin1_active", ui->multin1_active->isChecked());
	settings.setValue("multin2_active", ui->multin2_active->isChecked());
	settings.setValue("multin3_active", ui->multin3_active->isChecked());
	settings.setValue("multin4_active", ui->multin4_active->isChecked());
	settings.setValue("aux_active", ui->aux_active->isChecked());
	
	settings.setValue("in1_mode", ui->in1_mode->currentIndex());
	settings.setValue("in1_hp", ui->in1_hp->currentIndex());
	settings.setValue("in1_lp", ui->in1_lp->currentIndex());
	settings.setValue("in2_mode", ui->in2_mode->currentIndex());
	settings.setValue("in2_hp", ui->in2_hp->currentIndex());
	settings.setValue("in2_lp", ui->in2_lp->currentIndex());
	settings.setValue("in3_mode", ui->in3_mode->currentIndex());
	settings.setValue("in3_hp", ui->in3_hp->currentIndex());
	settings.setValue("in3_lp", ui->in3_lp->currentIndex());
	settings.setValue("in4_mode", ui->in4_mode->currentIndex());
	settings.setValue("in4_hp", ui->in4_hp->currentIndex());
	settings.setValue("in4_lp", ui->in4_lp->currentIndex());
	settings.setValue("in5_mode", ui->in5_mode->currentIndex());
	settings.setValue("in5_hp", ui->in5_hp->currentIndex());
	settings.setValue("in5_lp", ui->in5_lp->currentIndex());
	settings.setValue("in6_mode", ui->in6_mode->currentIndex());
	settings.setValue("in6_hp", ui->in6_hp->currentIndex());
	settings.setValue("in6_lp", ui->in6_lp->currentIndex());
	settings.setValue("in7_mode", ui->in7_mode->currentIndex());
	settings.setValue("in7_hp", ui->in7_hp->currentIndex());
	settings.setValue("in7_lp", ui->in7_lp->currentIndex());
	settings.setValue("in8_mode", ui->in8_mode->currentIndex());
	settings.setValue("in8_hp", ui->in8_hp->currentIndex());
	settings.setValue("in8_lp", ui->in8_lp->currentIndex());
	settings.setValue("multin1_mode", ui->multin1_mode->currentIndex());
	settings.setValue("multin1_hp", ui->multin1_hp->currentIndex());
	settings.setValue("multin1_lp", ui->multin1_lp->currentIndex());
	settings.setValue("multin2_mode", ui->multin2_mode->currentIndex());
	settings.setValue("multin2_hp", ui->multin2_hp->currentIndex());
	settings.setValue("multin2_lp", ui->multin2_lp->currentIndex());
	settings.setValue("multin3_mode", ui->multin3_mode->currentIndex());
	settings.setValue("multin3_hp", ui->multin3_hp->currentIndex());
	settings.setValue("multin3_lp", ui->multin3_lp->currentIndex());
	settings.setValue("multin4_mode", ui->multin4_mode->currentIndex());
	settings.setValue("multin4_hp", ui->multin4_hp->currentIndex());
	settings.setValue("multin4_lp", ui->multin4_lp->currentIndex());

	// analog output configuration
	settings.setValue("source_connector", ui->source_connector->currentIndex());
	settings.setValue("source_channel", ui->source_channel->currentIndex());
	settings.setValue("output_gain", ui->output_gain->currentIndex());
	settings.sync();
}


uint16_t MainWindow::convertSamplingFrequencyIndex(int index) const {
	switch (index) {
	case 0: return 512;
	case 1: return 2048;
	case 2: return 5120;
	case 3: return 10240;
	default:
		qDebug() << "unknown sampling frequency index";
		throw QException();
	}
}


InputMode MainWindow::convertInputModeIndex(int index) const {
	switch (index) {
	case 0: return InputMode::monopolar;
	case 1: return InputMode::differential;
	default:
		qDebug() << "unknown input mode index";
		throw QException();
	}
}


uint16_t MainWindow::convertHighpassIndex(int index) const {
	switch (index) {
	case 0: return 0; // this encodes actually 0.7 Hz
	case 1: return 10;
	case 2: return 100;
	case 3: return 200; // not shown in interface
	default:
		qDebug() << "unknown highpass index";
		throw QException();
	}
}


uint16_t MainWindow::convertLowpassIndex(int index) const {
	switch (index) {
	case 0: return 130;
	case 1: return 500;
	case 2: return 900;
	case 3: return 4400;
	default:
		qDebug() << "unknown lowpass index";
		throw QException();
	}
}


Connector MainWindow::convertInputConnectorIndex(int index) const {
	switch (index) {
	case 0: return Connector::in1;
	case 1: return Connector::in2;
	case 2: return Connector::in3;
	case 3: return Connector::in4;
	case 4: return Connector::in5;
	case 5: return Connector::in6;
	case 6: return Connector::in7;
	case 7: return Connector::in8;
	case 8: return Connector::multin1;
	case 9: return Connector::multin2;
	case 10: return Connector::multin3;
	case 11: return Connector::multin4;
	case 12: return Connector::auxin;
	default:
		qDebug() << "unknown connector index";
		throw QException();
	}
}


// updates the list of possible entries in a ComboBox
void MainWindow::updateComoBoxWithMaxFilterIndex(QComboBox* box, int max_index, QList<QString>& full_list) {
	// build list of possible entries with respect to the current selected sampling rate
	QList<QString> list_entries;
	for (int idx = 0; idx <= max_index; idx++)
		list_entries.insert(idx, full_list[idx]);

	// rebuild ComboBox
	int current_idx = box->currentIndex();
	box->clear();
	box->insertItems(0, list_entries);

	// select current index of ComboBox
	if (current_idx > max_index)
		box->setCurrentIndex(max_index);
	else
		box->setCurrentIndex(current_idx);
}


// the close event
// to avoid accidentally closing the window, we can ignore the close event
// when there's a recording in progress
void MainWindow::closeEvent(QCloseEvent *ev) {
	if (reader) {
		QMessageBox::warning(this, "Recording still running", "Can't quit while recording");
		ev->ignore();
	}
}


// toggling the recording state
// our record button has two functions: start a recording and
// stop it if a recording is running already.
void MainWindow::toggleRecording() {
	if (!reader) {
		std::string name = ui->stream_name->text().toStdString();

		ReaderParameters params;

		params.ip_address = QHostAddress(ui->ip->text());
		if (params.ip_address.isNull()) {
			QMessageBox::warning(this, "Error", "The IP adress is not valid. Please correct it before starting the recording.");
			return;
		}

		params.fs = convertSamplingFrequencyIndex(ui->sampling_frequency->currentIndex());

		params.in_active[0] = ui->in1_active->isChecked();
		params.in_active[1] = ui->in2_active->isChecked();
		params.in_active[2] = ui->in3_active->isChecked();
		params.in_active[3] = ui->in4_active->isChecked();
		params.in_active[4] = ui->in5_active->isChecked();
		params.in_active[5] = ui->in6_active->isChecked();
		params.in_active[6] = ui->in7_active->isChecked();
		params.in_active[7] = ui->in8_active->isChecked();
		params.multin_active[0] = ui->multin1_active->isChecked();
		params.multin_active[1] = ui->multin2_active->isChecked();
		params.multin_active[2] = ui->multin3_active->isChecked();
		params.multin_active[3] = ui->multin4_active->isChecked();
		params.aux_active = ui->aux_active->isChecked();

		params.in_mode[0] = convertInputModeIndex(ui->in1_mode->currentIndex());
		params.in_mode[1] = convertInputModeIndex(ui->in2_mode->currentIndex());
		params.in_mode[2] = convertInputModeIndex(ui->in3_mode->currentIndex());
		params.in_mode[3] = convertInputModeIndex(ui->in4_mode->currentIndex());
		params.in_mode[4] = convertInputModeIndex(ui->in5_mode->currentIndex());
		params.in_mode[5] = convertInputModeIndex(ui->in6_mode->currentIndex());
		params.in_mode[6] = convertInputModeIndex(ui->in7_mode->currentIndex());
		params.in_mode[7] = convertInputModeIndex(ui->in8_mode->currentIndex());
		params.multin_mode[0] = convertInputModeIndex(ui->multin1_mode->currentIndex());
		params.multin_mode[1] = convertInputModeIndex(ui->multin2_mode->currentIndex());
		params.multin_mode[2] = convertInputModeIndex(ui->multin3_mode->currentIndex());
		params.multin_mode[3] = convertInputModeIndex(ui->multin4_mode->currentIndex());

		params.in_hp[0] = convertHighpassIndex(ui->in1_hp->currentIndex());
		params.in_hp[1] = convertHighpassIndex(ui->in2_hp->currentIndex());
		params.in_hp[2] = convertHighpassIndex(ui->in3_hp->currentIndex());
		params.in_hp[3] = convertHighpassIndex(ui->in4_hp->currentIndex());
		params.in_hp[4] = convertHighpassIndex(ui->in5_hp->currentIndex());
		params.in_hp[5] = convertHighpassIndex(ui->in6_hp->currentIndex());
		params.in_hp[6] = convertHighpassIndex(ui->in7_hp->currentIndex());
		params.in_hp[7] = convertHighpassIndex(ui->in8_hp->currentIndex());
		params.multin_hp[0] = convertHighpassIndex(ui->multin1_hp->currentIndex());
		params.multin_hp[1] = convertHighpassIndex(ui->multin2_hp->currentIndex());
		params.multin_hp[2] = convertHighpassIndex(ui->multin3_hp->currentIndex());
		params.multin_hp[3] = convertHighpassIndex(ui->multin4_hp->currentIndex());

		params.in_lp[0] = convertLowpassIndex(ui->in1_lp->currentIndex());
		params.in_lp[1] = convertLowpassIndex(ui->in2_lp->currentIndex());
		params.in_lp[2] = convertLowpassIndex(ui->in3_lp->currentIndex());
		params.in_lp[3] = convertLowpassIndex(ui->in4_lp->currentIndex());
		params.in_lp[4] = convertLowpassIndex(ui->in5_lp->currentIndex());
		params.in_lp[5] = convertLowpassIndex(ui->in6_lp->currentIndex());
		params.in_lp[6] = convertLowpassIndex(ui->in7_lp->currentIndex());
		params.in_lp[7] = convertLowpassIndex(ui->in8_lp->currentIndex());
		params.multin_lp[0] = convertLowpassIndex(ui->multin1_lp->currentIndex());
		params.multin_lp[1] = convertLowpassIndex(ui->multin2_lp->currentIndex());
		params.multin_lp[2] = convertLowpassIndex(ui->multin3_lp->currentIndex());
		params.multin_lp[3] = convertLowpassIndex(ui->multin4_lp->currentIndex());

		params.source_connector = convertInputConnectorIndex(ui->source_connector->currentIndex());
		params.source_channel = ui->source_channel->currentIndex() + 1;
		params.output_gain = ui->output_gain->currentText().toInt();

		shutdown = false;
		reader = std::make_unique<std::thread>(&recording_thread_function, this, name, params, std::ref(shutdown));
		ui->linkButton->setText("Stop Recording");
		ui->linkButton->setChecked(true);
	} else {
		shutdown = true;
		reader->join();
		reader.reset();
		ui->linkButton->setText("Start Recording");
		ui->linkButton->setChecked(false);
	}
}


// update input configuration based on number of selected input channels
void MainWindow::updateInputConfiguration()
{
	ui->in1_mode->setEnabled(ui->in1_active->isChecked());
	ui->in1_hp->setEnabled(ui->in1_active->isChecked());
	ui->in1_lp->setEnabled(ui->in1_active->isChecked());
	ui->in2_mode->setEnabled(ui->in2_active->isChecked());
	ui->in2_hp->setEnabled(ui->in2_active->isChecked());
	ui->in2_lp->setEnabled(ui->in2_active->isChecked());
	ui->in3_mode->setEnabled(ui->in3_active->isChecked());
	ui->in3_hp->setEnabled(ui->in3_active->isChecked());
	ui->in3_lp->setEnabled(ui->in3_active->isChecked());
	ui->in4_mode->setEnabled(ui->in4_active->isChecked());
	ui->in4_hp->setEnabled(ui->in4_active->isChecked());
	ui->in4_lp->setEnabled(ui->in4_active->isChecked());
	ui->in5_mode->setEnabled(ui->in5_active->isChecked());
	ui->in5_hp->setEnabled(ui->in5_active->isChecked());
	ui->in5_lp->setEnabled(ui->in5_active->isChecked());
	ui->in6_mode->setEnabled(ui->in6_active->isChecked());
	ui->in6_hp->setEnabled(ui->in6_active->isChecked());
	ui->in6_lp->setEnabled(ui->in6_active->isChecked());
	ui->in7_mode->setEnabled(ui->in7_active->isChecked());
	ui->in7_hp->setEnabled(ui->in7_active->isChecked());
	ui->in7_lp->setEnabled(ui->in7_active->isChecked());
	ui->in8_mode->setEnabled(ui->in8_active->isChecked());
	ui->in8_hp->setEnabled(ui->in8_active->isChecked());
	ui->in8_lp->setEnabled(ui->in8_active->isChecked());
	ui->multin1_mode->setEnabled(ui->multin1_active->isChecked());
	ui->multin1_hp->setEnabled(ui->multin1_active->isChecked());
	ui->multin1_lp->setEnabled(ui->multin1_active->isChecked());
	ui->multin2_mode->setEnabled(ui->multin2_active->isChecked());
	ui->multin2_hp->setEnabled(ui->multin2_active->isChecked());
	ui->multin2_lp->setEnabled(ui->multin2_active->isChecked());
	ui->multin3_mode->setEnabled(ui->multin3_active->isChecked());
	ui->multin3_hp->setEnabled(ui->multin3_active->isChecked());
	ui->multin3_lp->setEnabled(ui->multin3_active->isChecked());
	ui->multin4_mode->setEnabled(ui->multin4_active->isChecked());
	ui->multin4_hp->setEnabled(ui->multin4_active->isChecked());
	ui->multin4_lp->setEnabled(ui->multin4_active->isChecked());
}


// update possible low/high pass filter settings after sampling frequency update
void MainWindow::updateLowHighPass() {
	int sampling_frequency_idx = ui->sampling_frequency->currentIndex();
	int max_lowpass_idx, max_highpass_idx;
	
	// determine max lowpass/highpass indices
	switch (sampling_frequency_idx) {
	case 0:
		max_lowpass_idx = 0;
		max_highpass_idx = 1;
		break;
	case 1:
		max_lowpass_idx = 2;
		max_highpass_idx = 2;
		break;
	case 2:
		max_lowpass_idx = 2;
		max_highpass_idx = 2;
		break;
	case 3:
		max_lowpass_idx = 3;
		max_highpass_idx = 2;
		break;
	default:
		qDebug() << "unknown sampling frequency index";
		throw QException();
	}

	QList<QString> full_lowpass_list = {"130 Hz", "500 Hz", "900 Hz", "4400 Hz"};
	// QList<QString> full_highpass_list = { "0.7 Hz", "10 Hz", "100 Hz", "200 Hz" }; --> 200 Hz HP could collide with 130 LP, so let's omit it
	QList<QString> full_highpass_list = {"0.7 Hz", "10 Hz", "100 Hz"};

	// update comboboxes with possible settings
	updateComoBoxWithMaxFilterIndex(ui->in1_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in1_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in2_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in2_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in3_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in3_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in4_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in4_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in5_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in5_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in6_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in6_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in7_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in7_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in8_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->in8_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin1_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin1_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin2_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin2_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin3_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin3_lp, max_lowpass_idx, full_lowpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin4_hp, max_highpass_idx, full_highpass_list);
	updateComoBoxWithMaxFilterIndex(ui->multin4_lp, max_lowpass_idx, full_lowpass_list);
}


// update possible source channels with respect to selected input connector
void MainWindow::updateSourceChannel() {
	int source_connector_idx = ui->source_connector->currentIndex();
	int n_channels;
	
	// get possible input channels
	if (source_connector_idx < 8) n_channels = 16; // IN1...IN8
	else if (source_connector_idx < 12) n_channels = 64; // MULTIN1...MULTIN4
	else if (source_connector_idx == 12) n_channels = 16; // AUX
	else {
		qDebug() << "unknown source connector index";
		throw QException();
	}

	// update combobox
	ui->source_channel->clear();
	for (int channel = 1; channel <= n_channels; channel++)
		ui->source_channel->addItem(QString::number(channel));
}


// error reporting
void MainWindow::showRecordingError(QString error_msg)
{
	shutdown = true;
	reader->join();
	reader.reset();
	ui->linkButton->setText("Start Recording");
	ui->linkButton->setChecked(false);
	QMessageBox::critical(this, "Error", "an error with the amplifier connection occured: " + error_msg);
}


 /* find a config file to load. This is (in descending order or preference):
 * - a file supplied on the command line
 * - [executablename].cfg in one the the following folders:
 * - the current working directory
 * - the default config folder, e.g. '~/Library/Preferences' on OS X
 * - the executable folder
 */
QString MainWindow::find_config_file(const char *filename) {
	if (filename) {
		QString qfilename(filename);
		if (!QFileInfo::exists(qfilename))
			QMessageBox(QMessageBox::Warning, "Config file not found",
				QStringLiteral("The file '%1' doesn't exist").arg(qfilename), QMessageBox::Ok,
				this);
		else
			return qfilename;
	}
	QFileInfo exeInfo(QCoreApplication::applicationFilePath());
	QString defaultCfgFilename(exeInfo.completeBaseName() + ".cfg");
	QStringList cfgpaths;
	cfgpaths << QDir::currentPath()
			 << QStandardPaths::standardLocations(QStandardPaths::ConfigLocation) << exeInfo.path();
	for (auto path : cfgpaths) {
		QString cfgfilepath = path + QDir::separator() + defaultCfgFilename;
		if (QFileInfo::exists(cfgfilepath)) return cfgfilepath;
	}
	QMessageBox(QMessageBox::Warning, "No config file not found",
		QStringLiteral("No default config file could be found"), QMessageBox::Ok, this);
	return "";
}


// tell the compiler to put the default destructor in this object file
MainWindow::~MainWindow() noexcept = default;
