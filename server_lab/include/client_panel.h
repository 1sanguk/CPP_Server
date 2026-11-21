#pragma once

#include <QList>
#include <QPair>
#include <QWidget>

class QLabel;
class QComboBox;
class QGridLayout;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

// 버튼·입력칸을 키우고 주요 버튼을 강조하는 Server Lab 공통 스타일. 메인 창과 클라이언트 화면이 함께 쓴다.
QString Lab_Style_Sheet();

// 클라이언트 테스트 화면. 위에서부터 "지금 할 일" 안내, 기대 결과, 연결 줄, 보내기 줄, 클라이언트 카드 격자 순이다.
class ClientPanel : public QWidget{
public:
    explicit ClientPanel(QWidget* parent = nullptr);
    ~ClientPanel() override;
    void Set_Room_Active(bool active);
    // v7부터는 길이 헤더 패킷 프로토콜을 사용하므로 실험방 버전에 맞춰 클라이언트 동작을 바꾼다.
    void Set_Server_Version(int version);
    void Shutdown();
    // 모든 카드의 보냄/받음 칸에 검색 조건을 적용한다. 나중에 연결한 카드에도 같은 조건이 적용된다.
    void Apply_Search(const QString& query, bool only_matches);
    // 카드별 (이름, 일치한 줄 수)
    QList<QPair<QString, int>> Search_Matches() const;

private:
    bool Uses_Packet_Protocol() const;
    // 선택한 버전에서 어떤 결과가 정상인지 설명하는 문구
    QString Expectation() const;
    // 현재 단계(방 시작 전 / 연결 전 / 연결 후)에 맞춰 안내 문구를 바꾼다.
    void Update_Guide();

    void Create_Clients();
    // 카드를 "한 줄에 카드" 수에 맞춰 격자에 다시 놓는다.
    void Layout_Cards();
    void Disconnect_Clients();
    // '보내는 쪽' 선택에 해당하는 카드들(모든 클라이언트 또는 하나)
    QList<QWidget*> Senders() const;
    void Send_Once();
    void Set_Auto_Send(bool on);
    // 카드 제목(상태, 보냄/받음)과 상단 요약을 갱신한다.
    void Refresh_Cards();

    QLabel* guide_label;
    QLabel* expect_label;
    QSpinBox* client_count;
    QPushButton* connect_button;
    QPushButton* disconnect_button;

    QWidget* send_box;
    QComboBox* sender_combo;
    QLineEdit* message_edit;
    QPushButton* random_button;
    QPushButton* auto_button;
    QPushButton* move_button;
    QPushButton* stop_move_button;

    QLabel* summary_label;
    QSpinBox* cards_per_row;
    QWidget* card_container;
    QLabel* empty_hint;
    QGridLayout* card_grid;
    QTimer* refresh_timer;

    // 클라이언트 카드. 소유는 Qt 부모-자식 관계가 하고, 여기는 순서(번호)만 기억한다.
    QList<QWidget*> cards;
    QString search_query;
    bool search_only_matches = false;
    int server_version = 0;
    bool room_active = false;
    bool shutting_down = false;
};
