#include <QtGui>
#include "tab_game.h"
#include "cardinfowidget.h"
#include "playerlistwidget.h"
#include "messagelogwidget.h"
#include "phasestoolbar.h"
#include "gameview.h"
#include "gamescene.h"
#include "player.h"
#include "zoneviewzone.h"
#include "zoneviewwidget.h"
#include "zoneviewlayout.h"
#include "deckview.h"
#include "decklist.h"
#include "deck_picturecacher.h"
#include "protocol_items.h"
#include "dlg_load_remote_deck.h"
#include "client.h"
#include "carditem.h"
#include "arrowitem.h"
#include "main.h"

TabGame::TabGame(Client *_client, int _gameId, int _localPlayerId, bool _spectator)
	: client(_client), gameId(_gameId), localPlayerId(_localPlayerId), spectator(_spectator), started(false), currentPhase(-1)
{
	zoneLayout = new ZoneViewLayout;
	scene = new GameScene(zoneLayout, this);
	gameView = new GameView(scene);
	gameView->hide();
	
	loadLocalButton = new QPushButton;
	loadRemoteButton = new QPushButton;
	readyStartButton = new QPushButton;
	
	QHBoxLayout *buttonHBox = new QHBoxLayout;
	buttonHBox->addWidget(loadLocalButton);
	buttonHBox->addWidget(loadRemoteButton);
	buttonHBox->addWidget(readyStartButton);
	buttonHBox->addStretch();
	deckView = new DeckView;
	QVBoxLayout *deckViewLayout = new QVBoxLayout;
	deckViewLayout->addLayout(buttonHBox);
	deckViewLayout->addWidget(deckView);
	deckViewContainer = new QWidget;
	deckViewContainer->setLayout(deckViewLayout);

	cardInfo = new CardInfoWidget;
	playerListWidget = new PlayerListWidget;
	messageLog = new MessageLogWidget;
	sayLabel = new QLabel;
	sayEdit = new QLineEdit;
	sayLabel->setBuddy(sayEdit);
	
	QHBoxLayout *hLayout = new QHBoxLayout;
	hLayout->addWidget(sayLabel);
	hLayout->addWidget(sayEdit);

	phasesToolbar = new PhasesToolbar;
	phasesToolbar->hide();
	connect(phasesToolbar, SIGNAL(sendGameCommand(GameCommand *)), this, SLOT(sendGameCommand(GameCommand *)));
	
	QVBoxLayout *verticalLayout = new QVBoxLayout;
	verticalLayout->addWidget(cardInfo);
	verticalLayout->addWidget(playerListWidget);
	verticalLayout->addWidget(messageLog);
	verticalLayout->addLayout(hLayout);

	QHBoxLayout *mainLayout = new QHBoxLayout;
	mainLayout->addWidget(phasesToolbar);
	mainLayout->addWidget(gameView, 10);
	mainLayout->addWidget(deckViewContainer, 10);
	mainLayout->addLayout(verticalLayout);

	aCloseMostRecentZoneView = new QAction(this);
	connect(aCloseMostRecentZoneView, SIGNAL(triggered()), zoneLayout, SLOT(closeMostRecentZoneView()));
	addAction(aCloseMostRecentZoneView);
	
	connect(loadLocalButton, SIGNAL(clicked()), this, SLOT(loadLocalDeck()));
	connect(loadRemoteButton, SIGNAL(clicked()), this, SLOT(loadRemoteDeck()));
	connect(readyStartButton, SIGNAL(clicked()), this, SLOT(readyStart()));
	
	connect(sayEdit, SIGNAL(returnPressed()), this, SLOT(actSay()));

	// Menu actions
	aNextPhase = new QAction(this);
	connect(aNextPhase, SIGNAL(triggered()), this, SLOT(actNextPhase()));
	aNextTurn = new QAction(this);
	connect(aNextTurn, SIGNAL(triggered()), this, SLOT(actNextTurn()));
	aRemoveLocalArrows = new QAction(this);
	connect(aRemoveLocalArrows, SIGNAL(triggered()), this, SLOT(actRemoveLocalArrows()));
	
	gameMenu = new QMenu(this);
	gameMenu->addAction(aNextPhase);
	gameMenu->addAction(aNextTurn);
	gameMenu->addSeparator();
	gameMenu->addAction(aRemoveLocalArrows);
	
	retranslateUi();
	setLayout(mainLayout);
	
	messageLog->logGameJoined(gameId);
}

void TabGame::retranslateUi()
{
	gameMenu->setTitle(tr("&Game"));
	aNextPhase->setText(tr("Next &phase"));
	aNextPhase->setShortcut(tr("Ctrl+Space"));
	aNextTurn->setText(tr("Next &turn"));
	aNextTurn->setShortcuts(QList<QKeySequence>() << QKeySequence(tr("Ctrl+Return")) << QKeySequence(tr("Ctrl+Enter")));
	aRemoveLocalArrows->setText(tr("&Remove all local arrows"));
	aRemoveLocalArrows->setShortcut(tr("Ctrl+R"));
	
	loadLocalButton->setText(tr("Load &local deck"));
	loadRemoteButton->setText(tr("Load deck from &server"));
	readyStartButton->setText(tr("&Start game"));
	sayLabel->setText(tr("&Say:"));
	cardInfo->retranslateUi();
	zoneLayout->retranslateUi();
	aCloseMostRecentZoneView->setText(tr("Close most recent zone view"));
	aCloseMostRecentZoneView->setShortcut(tr("Esc"));

	QMapIterator<int, Player *> i(players);
	while (i.hasNext())
		i.next().value()->retranslateUi();
}

void TabGame::actSay()
{
	if (!sayEdit->text().isEmpty()) {
		sendGameCommand(new Command_Say(-1, sayEdit->text()));
		sayEdit->clear();
	}
}

void TabGame::actNextPhase()
{
	int phase = currentPhase;
	if (++phase >= phasesToolbar->phaseCount())
		phase = 0;
	sendGameCommand(new Command_SetActivePhase(-1, phase));
}

void TabGame::actNextTurn()
{
	sendGameCommand(new Command_NextTurn);
}

void TabGame::actRemoveLocalArrows()
{
	QMapIterator<int, Player *> playerIterator(players);
	while (playerIterator.hasNext()) {
		Player *player = playerIterator.next().value();
		if (!player->getLocal())
			continue;
		QMapIterator<int, ArrowItem *> arrowIterator(player->getArrows());
		while (arrowIterator.hasNext()) {
			ArrowItem *a = arrowIterator.next().value();
			sendGameCommand(new Command_DeleteArrow(-1, a->getId()));
		}
	}
}

Player *TabGame::addPlayer(int playerId, const QString &playerName)
{
	Player *newPlayer = new Player(playerName, playerId, playerId == localPlayerId, client, this);
	scene->addPlayer(newPlayer);

	connect(newPlayer, SIGNAL(newCardAdded(CardItem *)), this, SLOT(newCardAdded(CardItem *)));
	messageLog->connectToPlayer(newPlayer);

	players.insert(playerId, newPlayer);
	emit playerAdded(newPlayer);

	return newPlayer;
}

void TabGame::processGameEvent(GameEvent *event)
{
	switch (event->getItemId()) {
		case ItemId_Event_GameStart: eventGameStart(qobject_cast<Event_GameStart *>(event)); break;
		case ItemId_Event_GameStateChanged: eventGameStateChanged(qobject_cast<Event_GameStateChanged *>(event)); break;
		case ItemId_Event_Join: eventJoin(qobject_cast<Event_Join *>(event)); break;
		case ItemId_Event_Leave: eventLeave(qobject_cast<Event_Leave *>(event)); break;
		case ItemId_Event_GameClosed: eventGameClosed(qobject_cast<Event_GameClosed *>(event)); break;
		case ItemId_Event_SetActivePlayer: eventSetActivePlayer(qobject_cast<Event_SetActivePlayer *>(event)); break;
		case ItemId_Event_SetActivePhase: eventSetActivePhase(qobject_cast<Event_SetActivePhase *>(event)); break;
		default: {
			Player *player = players.value(event->getPlayerId(), 0);
			if (!player) {
				qDebug() << "unhandled game event: invalid player id";
				break;
			}
			player->processGameEvent(event);
		}
	}
}

void TabGame::sendGameCommand(GameCommand *command)
{
	command->setGameId(gameId);
	client->sendCommand(command);
}

void TabGame::eventGameStart(Event_GameStart * /*event*/)
{
	currentPhase = -1;
	
	deckViewContainer->hide();
	gameView->show();
	phasesToolbar->show();
	messageLog->logGameStart();
	
	QMapIterator<int, Player *> i(players);
	while (i.hasNext())
		i.next().value()->prepareForGame();
}

void TabGame::eventGameStateChanged(Event_GameStateChanged *event)
{
	const QList<ServerInfo_Player *> &plList = event->getPlayerList();
	for (int i = 0; i < plList.size(); ++i) {
		ServerInfo_Player *pl = plList[i];
		Player *player = players.value(pl->getPlayerId(), 0);
		if (!player) {
			player = addPlayer(pl->getPlayerId(), pl->getName());
			playerListWidget->addPlayer(pl);
		}
		player->processPlayerInfo(pl);
	}
}

void TabGame::eventJoin(Event_Join *event)
{
	if (event->getSpectator()) {
		spectatorList.append(event->getPlayerName());
		messageLog->logJoinSpectator(event->getPlayerName());
	} else {
		Player *newPlayer = addPlayer(event->getPlayerId(), event->getPlayerName());
		messageLog->logJoin(newPlayer);
	}
}

void TabGame::eventLeave(Event_Leave *event)
{
	Player *player = players.value(event->getPlayerId(), 0);
	if (!player)
		return;
	
	messageLog->logLeave(player);
}

void TabGame::eventGameClosed(Event_GameClosed * /*event*/)
{
	started = false;
	messageLog->logGameClosed();
}

void TabGame::eventSetActivePlayer(Event_SetActivePlayer *event)
{
	Player *player = players.value(event->getActivePlayerId(), 0);
	if (!player)
		return;
	playerListWidget->setActivePlayer(event->getActivePlayerId());
	QMapIterator<int, Player *> i(players);
	while (i.hasNext()) {
		i.next();
		i.value()->setActive(i.value() == player);
	}
	messageLog->logSetActivePlayer(player);
	currentPhase = -1;
}

void TabGame::eventSetActivePhase(Event_SetActivePhase *event)
{
	const int phase = event->getPhase();
	if (currentPhase != phase) {
		currentPhase = phase;
		phasesToolbar->setActivePhase(phase);
		messageLog->logSetActivePhase(phase);
	}
}

void TabGame::loadLocalDeck()
{
	QFileDialog dialog(this, tr("Load deck"));
	QSettings settings;
	dialog.setDirectory(settings.value("paths/decks").toString());
	dialog.setNameFilters(DeckList::fileNameFilters);
	if (!dialog.exec())
		return;

	QString fileName = dialog.selectedFiles().at(0);
	DeckList::FileFormat fmt = DeckList::getFormatFromNameFilter(dialog.selectedNameFilter());
	DeckList *deck = new DeckList;
	if (!deck->loadFromFile(fileName, fmt)) {
		delete deck;
		// Error message
		return;
	}
	
	Command_DeckSelect *cmd = new Command_DeckSelect(gameId, deck, -1);
	connect(cmd, SIGNAL(finished(ProtocolResponse *)), this, SLOT(deckSelectFinished(ProtocolResponse *)));
	client->sendCommand(cmd);
}

void TabGame::loadRemoteDeck()
{
	DlgLoadRemoteDeck dlg(client);
	if (dlg.exec()) {
		Command_DeckSelect *cmd = new Command_DeckSelect(gameId, 0, dlg.getDeckId());
		connect(cmd, SIGNAL(finished(ProtocolResponse *)), this, SLOT(deckSelectFinished(ProtocolResponse *)));
		client->sendCommand(cmd);
	}
}

void TabGame::deckSelectFinished(ProtocolResponse *r)
{
	Response_DeckDownload *resp = qobject_cast<Response_DeckDownload *>(r);
	if (!resp)
		return;
	Command_DeckSelect *cmd = static_cast<Command_DeckSelect *>(sender());
	delete cmd->getDeck();
	
	Deck_PictureCacher::cachePictures(resp->getDeck(), this);
	deckView->setDeck(resp->getDeck());
}

void TabGame::readyStart()
{
	client->sendCommand(new Command_ReadyStart(gameId));
}

void TabGame::newCardAdded(CardItem *card)
{
	connect(card, SIGNAL(hovered(CardItem *)), cardInfo, SLOT(setCard(CardItem *)));
}
