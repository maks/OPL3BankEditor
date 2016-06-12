/*
 * OPL Bank Editor by Wohlstand, a free tool for music bank editing
 * Copyright (c) 2016 Vitaly Novichkov <admin@wohlnet.ru>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>

#include <QMimeData>

#include "bank_editor.h"
#include "ui_bank_editor.h"
#include "ins_names.h"
#include "FileFormats/junlevizion.h"
#include "FileFormats/dmxopl2.h"
#include "common.h"
#include "version.h"

BankEditor::BankEditor(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::BankEditor)
{
    memset(&m_clipboard, 0, sizeof(FmBank::Instrument));
    m_curInst = NULL;
    m_curInstBackup = NULL;
    m_lock = false;

    ui->setupUi(this);
    ui->version->setText(QString("%1, v.%2").arg(PROGRAM_NAME).arg(VERSION));
    m_recentMelodicNote = ui->noteToTest->value();
    setMelodic();
    connect(ui->melodic,    SIGNAL(clicked(bool)),  this,   SLOT(setMelodic()));
    connect(ui->percussion, SIGNAL(clicked(bool)),  this,   SLOT(setDrums()));
    loadInstrument();

    m_buffer.resize(8192);
    m_buffer.fill(0, 8192);

    this->setWindowFlags(Qt::WindowTitleHint|Qt::WindowSystemMenuHint|
                         Qt::WindowCloseButtonHint|Qt::WindowMinimizeButtonHint);
    this->setFixedSize(this->window()->width(), this->window()->height());

    initAudio();
    loadSettings();
}

BankEditor::~BankEditor()
{
    m_pushTimer.stop();
    m_audioOutput->stop();
    m_generator->stop();
    delete m_audioOutput;
    delete m_generator;
    delete ui;
}

void BankEditor::loadSettings()
{
    QApplication::setOrganizationName(_COMPANY);
    QApplication::setOrganizationDomain(_PGE_URL);
    QApplication::setApplicationName("OPL FM Banks Editor");
    QSettings setup;
    ui->deepTremolo->setChecked(setup.value("deep-tremolo", false).toBool());
    ui->deepVibrato->setChecked(setup.value("deep-vibrato", false).toBool());
    m_recentPath = setup.value("recent-path").toString();
}

void BankEditor::saveSettings()
{
    QSettings setup;
    setup.setValue("deep-tremolo", ui->deepTremolo->isChecked());
    setup.setValue("deep-vibrato", ui->deepVibrato->isChecked());
    setup.setValue("recent-path", m_recentPath);
}



void BankEditor::closeEvent(QCloseEvent *event)
{
    if(!askForSaving())
    {
        event->ignore();
        return;
    }
    saveSettings();
}

void BankEditor::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
    {
        e->acceptProposedAction();
    }
}

void BankEditor::dropEvent(QDropEvent *e)
{
    this->raise();
    this->setFocus(Qt::ActiveWindowFocusReason);

    foreach(const QUrl &url, e->mimeData()->urls())
    {
        const QString &fileName = url.toLocalFile();
        if(openFile(fileName))
            break; //Only first valid file!
    }
}



void BankEditor::initFileData(QString &filePath)
{
    m_recentPath = filePath;
    if(!ui->instruments->selectedItems().isEmpty())
        on_instruments_currentItemChanged(ui->instruments->selectedItems().first(), NULL);
    else
        on_instruments_currentItemChanged(NULL, NULL);
    ui->currentFile->setText(filePath);
    m_bankBackup = m_bank;
    reloadInstrumentNames();
}

void BankEditor::reInitFileDataAfterSave(QString &filePath)
{
    ui->currentFile->setText(filePath);
    m_recentPath = filePath;
    m_bankBackup = m_bank;
}

static void ErrMessageO(QWidget *parent, QString errStr)
{
    QMessageBox::warning(parent,
                        BankEditor::tr("Can't open bank file!"),
                        BankEditor::tr("Can't open bank file because %1.").arg(errStr),
                        QMessageBox::Ok);
}

static void ErrMessageS(QWidget *parent, QString errStr)
{
    QMessageBox::warning(parent,
                        BankEditor::tr("Can't save bank file!"),
                        BankEditor::tr("Can't save bank file because %1.").arg(errStr),
                        QMessageBox::Ok);
}

bool BankEditor::openFile(QString filePath)
{
    char magic[32];
    getMagic(filePath, magic, 32);

    int err = FmBankFormatBase::ERR_UNSUPPORTED_FORMAT;

    //Check out for Junglevision file format
    if(JunleVizion::detect(magic))
        err = JunleVizion::loadFile(filePath, m_bank);

    //Check for DMX OPL2 file format
    else if(DmxOPL2::detect(magic))
        err = DmxOPL2::loadFile(filePath, m_bank);


    if(err != FmBankFormatBase::ERR_OK)
    {
        QString errText;
        switch(err)
        {
        case FmBankFormatBase::ERR_BADFORMAT:
            errText = tr("bad file format"); break;
        case FmBankFormatBase::ERR_NOFILE:
            errText = tr("can't open file"); break;
        case FmBankFormatBase::ERR_NOT_IMLEMENTED:
            errText = tr("reading of this format is not implemented yet"); break;
        case FmBankFormatBase::ERR_UNSUPPORTED_FORMAT:
            errText = tr("unsupported file format"); break;
        case FmBankFormatBase::ERR_UNKNOWN:
            errText = tr("unknown error occouped"); break;
        }
        ErrMessageO(this, errText);
        return false;
    } else {
        initFileData(filePath);
        return true;
    }

    return false;
}

bool BankEditor::saveFile(QString filePath)
{
    int err = FmBankFormatBase::ERR_UNSUPPORTED_FORMAT;

    //Check out for Junglevision file format
    if(hasExt(filePath, "op3"))
        err = JunleVizion::saveFile(filePath, m_bank);

    //Check for DMX OPL2 file format
    else if(hasExt(filePath, "op2")||
            hasExt(filePath, "htc")||
            hasExt(filePath, "hxn"))
        err = DmxOPL2::saveFile(filePath, m_bank);

    if(err != FmBankFormatBase::ERR_OK)
    {
        QString errText;
        switch(err)
        {
        case FmBankFormatBase::ERR_BADFORMAT:
            errText = tr("bad file format"); break;
        case FmBankFormatBase::ERR_NOFILE:
            errText = tr("can't open file for write"); break;
        case FmBankFormatBase::ERR_NOT_IMLEMENTED:
            errText = tr("writing into this format is not implemented yet"); break;
        case FmBankFormatBase::ERR_UNSUPPORTED_FORMAT:
            errText = tr("unknown file name extension, please define file name extension to choice target file format"); break;
        case FmBankFormatBase::ERR_UNKNOWN:
            errText = tr("unknown error occouped"); break;
        }
        ErrMessageS(this, errText);
        return false;
    } else {
        reInitFileDataAfterSave(filePath);
        return true;
    }

    return false;
}

bool BankEditor::saveFileAs()
{
    QString jv  = "JunleVision bank (*.op3)";
    QString dmx = "DMX Bank (*.op2 *.htc *.hxn)";
    QString filters =  jv+";;"
                      +dmx;

    QString selectedFilter;

    if(hasExt(m_recentPath, ".op3"))
        selectedFilter = jv;
    else
    if(hasExt(m_recentPath, ".op2")||
       hasExt(m_recentPath, ".htc")||
       hasExt(m_recentPath, ".hxn"))
        selectedFilter = dmx;

    QString fileToSave = QFileDialog::getSaveFileName(this, "Save bank file", m_recentPath, filters, &selectedFilter);

    if(fileToSave.isEmpty())
        return false;

    return saveFile(fileToSave);
}

bool BankEditor::askForSaving()
{
    if(m_bank != m_bankBackup)
    {
        QMessageBox::StandardButton res = QMessageBox::question(this, tr("File is not saved"), tr("File is modified and not saved. Do you want to save it?"), QMessageBox::Yes|QMessageBox::No|QMessageBox::Cancel);
        if((res==QMessageBox::Cancel) || (res==QMessageBox::NoButton))
        {
            return false;
        }
        else
        if(res==QMessageBox::Yes)
        {
            if(!saveFileAs())
            {
                return false;
            }
        }
    }
    return true;
}

void BankEditor::flushInstrument()
{
    loadInstrument();
    if( m_curInst && ui->percussion->isChecked() )
        ui->noteToTest->setValue( m_curInst->percNoteNum );
    sendPatch();
}

void BankEditor::on_actionNew_triggered()
{
    if( !askForSaving() )
        return;

    ui->currentFile->setText(tr("<Untitled>"));
    ui->instruments->clearSelection();
    m_bank.reset();
    m_bankBackup.reset();
    on_instruments_currentItemChanged(NULL, NULL);
    reloadInstrumentNames();
}

void BankEditor::on_actionOpen_triggered()
{
    if( !askForSaving() )
        return;

    QString supported   = "Supported bank files (*.op3 *.op2  *.htc *.hxn)";
    QString jv          = "JunleVision bank (*.op3)";
    QString dmx         = "DMX Bank (*.op2 *.htc *.hxn)";
    QString allFiles    = "All files (*.*)";

    QString filters =   supported+";;"
                       +jv+";;"
                       +dmx+";;"
                       +allFiles;

    QString fileToOpen;
    fileToOpen = QFileDialog::getOpenFileName(this, "Open bank file", m_recentPath, filters);
    if(fileToOpen.isEmpty())
        return;

    openFile(fileToOpen);
}

void BankEditor::on_actionSave_triggered()
{
    saveFileAs();
}

void BankEditor::on_actionExit_triggered()
{
    this->close();
}


void BankEditor::on_actionCopy_triggered()
{
    if(!m_curInst) return;
    memcpy(&m_clipboard, m_curInst, sizeof(FmBank::Instrument));
}

void BankEditor::on_actionPaste_triggered()
{
    if(!m_curInst) return;

    memcpy(m_curInst, &m_clipboard, sizeof(FmBank::Instrument));
    flushInstrument();
}

void BankEditor::on_actionReset_current_instrument_triggered()
{
    if(!m_curInstBackup || !m_curInst)
        return; //Some pointer is Null!!!

    if( memcmp(m_curInst, m_curInstBackup, sizeof(FmBank::Instrument))==0 )
        return; //Nothing to do

    if( QMessageBox::Yes == QMessageBox::question(this,
                                                  tr("Reset instrument to initial state"),
                                                  tr("This instrument will be reseted to initial state "
                                                     "(sice this file loaded or saved).\n"
                                                     "Are you wish to continue?"),
                                                  QMessageBox::Yes|QMessageBox::No) )
    {
        memcpy(m_curInst, m_curInstBackup, sizeof(FmBank::Instrument));
        flushInstrument();
    }
}


void BankEditor::on_actionAbout_triggered()
{
    QMessageBox::information(this,
                             tr("About bank editor"),
                             tr("FM Bank Editor for Yamaha OPL3/OPL2 chip, Version %1\n\n"
                                "(c) 2016, Vitaly Novichkov \"Wohlstand\"\n"
                                "\n"
                                "Licensed under GNU GPLv3\n\n"
                                "Source code available on GitHub:\n"
                                "%2").arg(VERSION).arg("https://github.com/Wohlstand/OPL3BankEditor"),
                             QMessageBox::Ok);
}

void BankEditor::on_instruments_currentItemChanged(QListWidgetItem *current, QListWidgetItem *)
{
    if(!current)
    {
        //ui->curInsInfo->setText("<Not Selected>");
        m_curInst = NULL;
    } else  {
        //ui->curInsInfo->setText(QString("%1 - %2").arg(current->data(Qt::UserRole).toInt()).arg(current->text()));
        setCurrentInstrument(current->data(Qt::UserRole).toInt(), ui->percussion->isChecked() );
    }
    flushInstrument();
}


void BankEditor::setCurrentInstrument(int num, bool isPerc)
{
    m_curInst = isPerc ? &m_bank.Ins_Percussion[num] : &m_bank.Ins_Melodic[num];
    m_curInstBackup = isPerc ? &m_bankBackup.Ins_Percussion[num] : &m_bankBackup.Ins_Melodic[num];
}

void BankEditor::loadInstrument()
{
    if(!m_curInst)
    {
        ui->editzone->setEnabled(false);
        ui->editzone2->setEnabled(false);
        ui->testNoteBox->setEnabled(false);
        ui->piano->setEnabled(false);
        m_lock = true;
        ui->insName->setEnabled(false);
        ui->insName->clear();
        m_lock = false;
        return;
    }

    ui->editzone->setEnabled(true);
    ui->editzone2->setEnabled(true);
    ui->testNoteBox->setEnabled(true);
    ui->piano->setEnabled(ui->melodic->isChecked());
    ui->insName->setEnabled(true);

    m_lock = true;
    ui->insName->setText( m_curInst->name );

    ui->perc_noteNum->setValue( m_curInst->percNoteNum );
    ui->op4mode->setChecked( m_curInst->en_4op );
    ui->doubleVoice->setEnabled( m_curInst->en_4op );
    ui->doubleVoice->setChecked( m_curInst->en_pseudo4op );
    ui->carrier2->setEnabled( m_curInst->en_4op );
    ui->modulator2->setEnabled( m_curInst->en_4op );
    ui->feedback2->setEnabled( m_curInst->en_4op );
    ui->connect2->setEnabled( m_curInst->en_4op );
    ui->feedback2label->setEnabled( m_curInst->en_4op );

    ui->feedback1->setValue( m_curInst->feedback1 );
    ui->feedback2->setValue( m_curInst->feedback2 );

    ui->secVoiceFineTune->setValue( m_curInst->fine_tune );

    ui->noteOffset1->setValue( m_curInst->note_offset1 );
    ui->noteOffset2->setValue( m_curInst->note_offset2 );

    ui->am1->setChecked( m_curInst->connection1==FmBank::Instrument::Connections::AM );
    ui->fm1->setChecked( m_curInst->connection1==FmBank::Instrument::Connections::FM );

    ui->am2->setChecked( m_curInst->connection2==FmBank::Instrument::Connections::AM );
    ui->fm2->setChecked( m_curInst->connection2==FmBank::Instrument::Connections::FM );


    ui->op1_attack->setValue(m_curInst->OP[MODULATOR1].attack);
    ui->op1_decay->setValue(m_curInst->OP[MODULATOR1].decay);
    ui->op1_sustain->setValue(m_curInst->OP[MODULATOR1].sustain);
    ui->op1_release->setValue(m_curInst->OP[MODULATOR1].release);
    ui->op1_waveform->setCurrentIndex(m_curInst->OP[MODULATOR1].waveform);
    ui->op1_freqmult->setValue(m_curInst->OP[MODULATOR1].fmult);
    ui->op1_level->setValue(m_curInst->OP[MODULATOR1].level);
    ui->op1_ksl->setValue(m_curInst->OP[MODULATOR1].ksl);
    ui->op1_vib->setChecked(m_curInst->OP[MODULATOR1].vib);
    ui->op1_am->setChecked(m_curInst->OP[MODULATOR1].am);
    ui->op1_eg->setChecked(m_curInst->OP[MODULATOR1].eg);
    ui->op1_ksr->setChecked(m_curInst->OP[MODULATOR1].ksr);

    ui->op2_attack->setValue(m_curInst->OP[CARRIER1].attack);
    ui->op2_decay->setValue(m_curInst->OP[CARRIER1].decay);
    ui->op2_sustain->setValue(m_curInst->OP[CARRIER1].sustain);
    ui->op2_release->setValue(m_curInst->OP[CARRIER1].release);
    ui->op2_waveform->setCurrentIndex(m_curInst->OP[CARRIER1].waveform);
    ui->op2_freqmult->setValue(m_curInst->OP[CARRIER1].fmult);
    ui->op2_level->setValue(m_curInst->OP[CARRIER1].level);
    ui->op2_ksl->setValue(m_curInst->OP[CARRIER1].ksl);
    ui->op2_vib->setChecked(m_curInst->OP[CARRIER1].vib);
    ui->op2_am->setChecked(m_curInst->OP[CARRIER1].am);
    ui->op2_eg->setChecked(m_curInst->OP[CARRIER1].eg);
    ui->op2_ksr->setChecked(m_curInst->OP[CARRIER1].ksr);

    ui->op3_attack->setValue(m_curInst->OP[MODULATOR2].attack);
    ui->op3_decay->setValue(m_curInst->OP[MODULATOR2].decay);
    ui->op3_sustain->setValue(m_curInst->OP[MODULATOR2].sustain);
    ui->op3_release->setValue(m_curInst->OP[MODULATOR2].release);
    ui->op3_waveform->setCurrentIndex(m_curInst->OP[MODULATOR2].waveform);
    ui->op3_freqmult->setValue(m_curInst->OP[MODULATOR2].fmult);
    ui->op3_level->setValue(m_curInst->OP[MODULATOR2].level);
    ui->op3_ksl->setValue(m_curInst->OP[MODULATOR2].ksl);
    ui->op3_vib->setChecked(m_curInst->OP[MODULATOR2].vib);
    ui->op3_am->setChecked(m_curInst->OP[MODULATOR2].am);
    ui->op3_eg->setChecked(m_curInst->OP[MODULATOR2].eg);
    ui->op3_ksr->setChecked(m_curInst->OP[MODULATOR2].ksr);

    ui->op4_attack->setValue(m_curInst->OP[CARRIER2].attack);
    ui->op4_decay->setValue(m_curInst->OP[CARRIER2].decay);
    ui->op4_sustain->setValue(m_curInst->OP[CARRIER2].sustain);
    ui->op4_release->setValue(m_curInst->OP[CARRIER2].release);
    ui->op4_waveform->setCurrentIndex(m_curInst->OP[CARRIER2].waveform);
    ui->op4_freqmult->setValue(m_curInst->OP[CARRIER2].fmult);
    ui->op4_level->setValue(m_curInst->OP[CARRIER2].level);
    ui->op4_ksl->setValue(m_curInst->OP[CARRIER2].ksl);
    ui->op4_vib->setChecked(m_curInst->OP[CARRIER2].vib);
    ui->op4_am->setChecked(m_curInst->OP[CARRIER2].am);
    ui->op4_eg->setChecked(m_curInst->OP[CARRIER2].eg);
    ui->op4_ksr->setChecked(m_curInst->OP[CARRIER2].ksr);

    m_lock = false;
}

void BankEditor::sendPatch()
{
    if(!m_curInst) return;
    if(!m_generator) return;
    m_generator->changePatch(*m_curInst);
}

void BankEditor::setDrumMode(bool dmode)
{
    if(dmode)
    {
        if(ui->noteToTest->isEnabled())
        {
            m_recentMelodicNote = ui->noteToTest->value();
        }
    } else {
        ui->noteToTest->setValue(m_recentMelodicNote);
    }
    ui->noteToTest->setDisabled(dmode);
    ui->testMajor->setDisabled(dmode);
    ui->testMinor->setDisabled(dmode);
    ui->testAugmented->setDisabled(dmode);
    ui->testDiminished->setDisabled(dmode);
    ui->testMajor7->setDisabled(dmode);
    ui->testMinor7->setDisabled(dmode);
    ui->piano->setDisabled(dmode);
}

void BankEditor::setMelodic()
{
    setDrumMode(false);
    ui->instruments->clear();
    for(int i=0; i<128; i++)
    {
        QListWidgetItem* item = new QListWidgetItem();
        item->setText(m_bank.Ins_Melodic[i].name[0]!='\0' ?
                            m_bank.Ins_Melodic[i].name : MidiInsName[i]);
        item->setData(Qt::UserRole, i);
        item->setToolTip(QString("ID: %1").arg(i));
        item->setFlags(Qt::ItemIsSelectable|Qt::ItemIsEnabled);
        ui->instruments->addItem(item);
    }
}

void BankEditor::setDrums()
{
    setDrumMode(true);
    ui->instruments->clear();
    for(int i=0; i<128; i++)
    {
        QListWidgetItem* item = new QListWidgetItem();
        item->setText(m_bank.Ins_Percussion[i].name[0]!='\0' ?
                            m_bank.Ins_Percussion[i].name : MidiPercName[i]);
        item->setData(Qt::UserRole, i);
        item->setToolTip(QString("ID: %1").arg(i));
        item->setFlags(Qt::ItemIsSelectable|Qt::ItemIsEnabled);
        ui->instruments->addItem(item);
    }
}

void BankEditor::reloadInstrumentNames()
{
    QList<QListWidgetItem*> items = ui->instruments->findItems("*", Qt::MatchWildcard);
    if(ui->percussion->isChecked())
    {
        for(int i=0; i<items.size(); i++)
        {
            int index = items[i]->data(Qt::UserRole).toInt();
            items[i]->setText( m_bank.Ins_Percussion[index].name[0]!='\0' ?
                                                m_bank.Ins_Percussion[index].name :
                                                MidiPercName[index] );
        }
    } else {
        for(int i=0; i<items.size(); i++)
        {
            int index = items[i]->data(Qt::UserRole).toInt();
            items[i]->setText( m_bank.Ins_Melodic[index].name[0]!='\0' ?
                                                m_bank.Ins_Melodic[index].name :
                                                MidiInsName[index] );
        }
    }
}
