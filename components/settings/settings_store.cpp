#include "settings_record.h"
#include "fram_device.h"
#include "media_records.h"
#include "parental_record.h"
template class Persistence::SettingsStore<Board::FramDevice>;
template class Persistence::MediaRecords<Board::FramDevice>;
template class Persistence::ParentalStore<Board::FramDevice>;
