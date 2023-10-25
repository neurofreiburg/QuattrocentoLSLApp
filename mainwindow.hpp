#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QComboBox>
#include <atomic>
#include <memory> //for std::unique_ptr
#include <thread>

#include "reader.hpp"


// to keep our include lists and compile times short we only provide forward
// declarations for classes we only have pointers to
namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(QWidget *parent, const char *config_file);
	~MainWindow() noexcept override;

private slots:
	void closeEvent(QCloseEvent *ev) override;
	void toggleRecording();
	void updateInputConfiguration();
	void updateLowHighPass();
	void updateSourceChannel();
	void showRecordingError(QString error_msg);

signals:
	void errorWhenRecording(QString error_msg);

private:
	// functions for loading/saving the config file
	QString find_config_file(const char *filename);
	void load_config(const QString &filename);
	void save_config(const QString &filename);

	uint16_t convertSamplingFrequencyIndex(int index) const;
	InputMode convertInputModeIndex(int index) const;
	uint16_t convertHighpassIndex(int index) const;
	uint16_t convertLowpassIndex(int index) const;
	Connector convertInputConnectorIndex(int index) const;

	void updateComoBoxWithMaxFilterIndex(QComboBox* box, int max_index, QList<QString>& full_list);

	std::unique_ptr<std::thread> reader{nullptr};

	std::unique_ptr<Ui::MainWindow> ui; // window pointer
	std::atomic<bool> shutdown{false};  // flag indicating whether the recording thread should quit
};

#endif // MAINWINDOW_H
