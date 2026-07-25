#pragma once

class OfficialGestureEntryGrace {
public:
    bool consume()
    {
        if (!pending_) {
            return false;
        }
        pending_ = false;
        return true;
    }

    void reset()
    {
        pending_ = true;
    }

    bool pending() const
    {
        return pending_;
    }

private:
    bool pending_ = true;
};
