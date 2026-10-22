#pragma once

enum class orderType{buy , sell};

struct Order{
    int orderId;
    orderType type;
    int limit;
    int shares;
};