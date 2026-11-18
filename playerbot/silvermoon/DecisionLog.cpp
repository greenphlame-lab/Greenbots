#include "playerbot/playerbot.h"
#include "DecisionLog.h"
#include "Database/DatabaseEnv.h"
#include <mutex>
#include <sstream>

namespace silvermoon
{
    namespace
    {
        const time_t FLUSH_SECONDS = 30;
        const size_t MAX_BUFFERED = 5000;   // safety cap if flushing stalls
        const size_t ROWS_PER_INSERT = 200;

        std::mutex bufferLock;
        std::vector<DecisionRow> buffer;
        time_t lastFlush = 0;

        // game link markup -> plain [name]: "|cFFFFFF00|Hentry:1866:|h[Ravenclaw Slave]|h|r" becomes
        // "[Ravenclaw Slave]", so titles stay readable and fit the 96-character columns
        std::string StripLinks(std::string const& text)
        {
            std::string out;
            size_t i = 0;
            while (i < text.size())
            {
                if (text.compare(i, 2, "|c") == 0)
                {
                    size_t open = text.find("|h[", i);
                    size_t close = open == std::string::npos ? std::string::npos : text.find("]|h", open + 3);
                    if (close != std::string::npos)
                    {
                        out += "[" + text.substr(open + 3, close - open - 3) + "]";
                        i = close + 3;
                        if (text.compare(i, 2, "|r") == 0)
                            i += 2;
                        continue;
                    }
                }
                out += text[i++];
            }
            return out;
        }

        std::string Quote(std::string text, size_t maxLength)
        {
            if (text.size() > maxLength)
                text.resize(maxLength);
            CharacterDatabase.escape_string(text);
            return "'" + text + "'";
        }

        std::string Number(float value)
        {
            std::ostringstream out;
            out.precision(4);
            out << std::fixed << value;
            return out.str();
        }
    }

    void DecisionLog::Add(DecisionRow row)
    {
        row.target = StripLinks(row.target);
        row.runnerUp1 = StripLinks(row.runnerUp1);
        row.runnerUp2 = StripLinks(row.runnerUp2);
        std::lock_guard<std::mutex> guard(bufferLock);
        if (buffer.size() < MAX_BUFFERED)
            buffer.push_back(std::move(row));
    }

    void DecisionLog::FlushIfDue()
    {
        time_t now = time(nullptr);
        if (now - lastFlush < FLUSH_SECONDS)
            return;
        lastFlush = now;
        Flush();
    }

    void DecisionLog::Flush()
    {
        std::vector<DecisionRow> rows;
        {
            std::lock_guard<std::mutex> guard(bufferLock);
            rows.swap(buffer);
        }
        for (size_t start = 0; start < rows.size(); start += ROWS_PER_INSERT)
        {
            std::ostringstream sql;
            sql << "INSERT INTO silvermoon_decisions (ts, guid, goal, target, score, runner_up1, runner_up1_score, runner_up2, runner_up2_score, reason, map, x, y) VALUES ";
            for (size_t i = start; i < rows.size() && i < start + ROWS_PER_INSERT; ++i)
            {
                DecisionRow const& r = rows[i];
                if (i > start)
                    sql << ",";
                sql << "(" << r.ts << "," << r.guid << "," << Quote(r.goal, 48) << "," << Quote(r.target, 96) << "," << Number(r.score) << ","
                    << Quote(r.runnerUp1, 96) << "," << Number(r.runnerUp1Score) << "," << Quote(r.runnerUp2, 96) << "," << Number(r.runnerUp2Score) << ","
                    << Quote(r.reason, 96) << "," << r.map << "," << Number(r.x) << "," << Number(r.y) << ")";
            }
            CharacterDatabase.Execute(sql.str().c_str());
        }
    }
}
