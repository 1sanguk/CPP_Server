#pragma once

#include <QListWidget>

class QKeyEvent;

// 한 줄 = 한 항목인 로그 목록. 클라이언트 카드의 보냄/받음 칸과 서버 로그가 함께 쓴다.
// - 검색어가 들어간 줄을 노란색으로 칠하고, "찾은 줄만 보기"면 나머지 줄을 숨긴다.
// - 새 줄이 들어와도 같은 검색 조건이 바로 적용된다.
// - 줄을 골라 Ctrl+C로 복사할 수 있다.
// - 오래된 줄부터 버려 메모리가 끝없이 늘지 않게 한다.
class LogList : public QListWidget{
public:
    explicit LogList(int max_lines, QWidget* parent = nullptr);

    void Add_Line(const QString& text, const QColor& color = QColor(), bool bold = false);
    // 검색 조건을 바꾸고 일치하는 줄 수를 돌려준다. 빈 검색어면 강조와 숨김을 모두 푼다.
    int Apply_Search(const QString& query, bool only_matches);
    int Match_Count() const{ return match_count; }

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    bool Matches(const QListWidgetItem* item) const;
    // 한 줄에 현재 검색 조건(강조, 숨김)을 적용하고 일치 여부를 돌려준다.
    bool Apply_To(QListWidgetItem* item) const;

    int max_lines;
    QString query;
    bool only_matches = false;
    int match_count = 0;
};
