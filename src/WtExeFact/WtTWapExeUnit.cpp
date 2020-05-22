#include "WtTWapExeUnit.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/WTSVariant.hpp"
#include "../Share/WTSContractInfo.hpp"
#include "../Share/decimal.h"

#include <math.h>


extern const char* FACT_NAME;


WtTWapExeUnit::WtTWapExeUnit()
	: _last_tick(NULL)
	, _comm_info(NULL)
	, _ord_sticky(0)
	, _cancel_cnt(0)
	, _channel_ready(false)
	, _last_fire_time(0)
	, _fired_times(0)
	, _total_times(0)
	, _total_secs(0)
	, _price_mode(0)
	, _price_offset(0)
	, _target_pos(0)
{
}


WtTWapExeUnit::~WtTWapExeUnit()
{
	if (_last_tick)
		_last_tick->release();

	if (_comm_info)
		_comm_info->release();
}

const char* WtTWapExeUnit::getFactName()
{
	return FACT_NAME;
}

const char* WtTWapExeUnit::getName()
{
	return "WtTWapExeUnit";
}

void WtTWapExeUnit::init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg)
{
	ExecuteUnit::init(ctx, stdCode, cfg);

	_comm_info = ctx->getCommodityInfo(stdCode);
	if (_comm_info)
		_comm_info->retain();

	_ord_sticky = cfg->getUInt32("ord_sticky");
	_total_secs = cfg->getUInt32("total_secs");
	_tail_secs = cfg->getUInt32("tail_secs");
	_total_times = cfg->getUInt32("total_times");
	_price_mode = cfg->getUInt32("price_mode");
	_price_offset = cfg->getUInt32("price_offset");

	_fire_span = (_total_secs - _tail_secs) / _total_times;		//���η���ʱ������Ҫȥ��β��ʱ����㣬�����Ļ������ʣ�����������һ�����׷����Ļ�����

	ctx->writeLog("ִ�е�ԪWtTWapExeUnit[%s] ��ʼ����ɣ�������ʱ %u �룬ִ��ʱ�� %u �룬��βʱ�� %u ��", stdCode, _ord_sticky, _total_secs, _tail_secs);
}

void WtTWapExeUnit::on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled)
{
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return;

	if (isCanceled || leftover == 0)
	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		_orders.erase(it);
		if (_cancel_cnt > 0)
			_cancel_cnt--;

		_ctx->writeLog("@ %d cancelcnt -> %u", __LINE__, _cancel_cnt);
	}

	//���ȫ�������ѳ��������ʱ��һ��������Ҫ��ʱ����
	if (isCanceled && _cancel_cnt == 0)
	{
		double realPos = _ctx->getPosition(stdCode);
		if(!decimal::eq(realPos, _this_target))
		{
			//�����Ժ��ط���һ���Ǽӵ��ط�
			fire_at_once(_this_target - realPos);
		}
	}
}

void WtTWapExeUnit::on_channel_ready()
{
	_channel_ready = true;
	double undone = _ctx->getUndoneQty(_code.c_str());
	if (undone != 0 && _orders.empty())
	{
		//��˵����δ��ɵ����ڼ��֮�У��ȳ���
		_ctx->writeLog("%s�в��ڹ����е�δ��ɵ� %.0f �֣�ȫ������", _code.c_str(), undone);

		bool isBuy = (undone > 0);
		OrderIDs ids = _ctx->cancel(_code.c_str(), isBuy);
		for (auto localid : ids)
		{
			_orders[localid] = _ctx->getCurTime();
		}
		_cancel_cnt += ids.size();

		_ctx->writeLog("%s cancelcnt -> %u", __FUNCTION__, _cancel_cnt);
	}


	do_calc();
}

void WtTWapExeUnit::on_channel_lost()
{

}

void WtTWapExeUnit::on_tick(WTSTickData* newTick)
{
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;

	bool isFirstTick = false;
	//���ԭ����tick��Ϊ�գ���Ҫ�ͷŵ�
	if (_last_tick)
		_last_tick->release();
	else
		isFirstTick = true;

	//�µ�tick���ݣ�Ҫ����
	_last_tick = newTick;
	_last_tick->retain();

	/*
	*	������Կ���һ��
	*	���д����һ�ζ���ȥ�ĵ��Ӳ����ﵽĿ���λ
	*	��ô���µ��������ݽ�����ʱ������ٴδ��������߼�
	*/

	if (isFirstTick)	//����ǵ�һ��tick������Ŀ���λ�����������µ�
	{
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();
		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode);
		if (!decimal::eq(newVol, undone + realPos)) //��λ�仯Ҫ����
		{
			do_calc();
		}
	}
	else
	{
		uint64_t now = _ctx->getCurTime();
		bool hasCancel = false;
		if (_ord_sticky != 0 && !_orders.empty())
		{
			StdLocker<StdRecurMutex> lock(_mtx_ords);
			for (auto v : _orders)
			{
				uint32_t localid = v.first;
				uint64_t firetime = v.second;
				if (now - firetime > _ord_sticky * 1000) //Ҫ�����ع�
				{
					if (_ctx->cancel(localid))
					{
						_cancel_cnt++;
						_ctx->writeLog("@ %d cancelcnt -> %u", __LINE__, _cancel_cnt);
						hasCancel = true;
					}
				}
			}
		}
		
		if (!hasCancel && (now - _last_fire_time >= _fire_span))
		{
			do_calc();
		}
	}
}

void WtTWapExeUnit::on_trade(const char* stdCode, bool isBuy, double vol, double price)
{
	//���ô�����������ontick�ﴥ����
	//_ctx->writeLog("%s��Լ%s%u�֣����´���ִ���߼�", stdCode, isBuy?"����":"����", vol);
	//doCalculate();
}

void WtTWapExeUnit::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	if (!bSuccess)
	{
		//��������ҷ���ȥ�Ķ������ҾͲ�����
		auto it = _orders.find(localid);
		if (it == _orders.end())
			return;

		_orders.erase(it);

		do_calc();
	}
}

void WtTWapExeUnit::fire_at_once(double qty)
{
	if (decimal::eq(qty, 0))
		return;

	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	const char* code = _code.c_str();
	uint64_t now = TimeUtils::getLocalTimeNow();
	bool isBuy = decimal::gt(qty, 0);
	double targetPx = 0;
	//���ݼ۸�ģʽ���ã�ȷ��ί�л�׼�۸�0-���¼ۣ�1-���żۣ�2-���ּ�
	if (_price_mode == 0)
	{
		targetPx = curTick->price();
	}
	else if (_price_mode == 1)
	{
		targetPx = isBuy ? curTick->bidprice(0) : curTick->askprice(0);
	}
	else // if(_price_mode == 2)
	{
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}

	//�����Ҫȫ����������۸�ƫ��5�����Ա���ִ��
	targetPx += _comm_info->getPriceTick() * 5 * (isBuy ? 1 : -1);

	//��󷢳�ָ��
	OrderIDs ids;
	if (qty > 0)
		ids = _ctx->buy(code, targetPx, abs(qty));
	else
		ids = _ctx->sell(code, targetPx, abs(qty));

	StdLocker<StdRecurMutex> lock(_mtx_ords);
	for (auto localid : ids)
	{
		_orders[localid] = now;
	}

	curTick->release();
}

void WtTWapExeUnit::do_calc()
{
	if (!_channel_ready)
		return;

	//�����ڳ����Ķ��������ܽ�����һ�ּ���
	if (_cancel_cnt != 0)
	{
		_ctx->writeLog("%s����δ��ɳ���ָ���ʱ�˳�����ִ��", _code.c_str());
		return;
	}

	if (_last_tick == NULL)
	{
		_ctx->writeLog("%sû������tick���ݣ��˳�ִ���߼�", _code.c_str());
		return;
	}

	const char* code = _code.c_str();

	double undone = _ctx->getUndoneQty(code);
	//ÿһ�η���Ҫ���ϳɽ������������δ��ɵ���˵����һ��û���
	if (undone != 0)
	{
		_ctx->writeLog("%s��һ���йҵ�δ��ɣ���ʱ�˳�����ִ��", _code.c_str());
		return;
	}

	double realPos = _ctx->getPosition(code);
	double diffQty = _target_pos - realPos;

	if (decimal::eq(diffQty, 0))
		return;

	uint32_t leftTimes = _total_times - _fired_times;

	bool bNeedShowHand = false;
	//ʣ�����Ϊ0��ʣ��������Ϊ0��˵��Ҫȫ������ȥ��
	//ʣ�����Ϊ0��˵���Ѿ����˶���ʱ���ˣ�������ʱ����δ�������������Ҫ����
	if (leftTimes == 0 && !decimal::eq(diffQty, 0))
		bNeedShowHand = true;

	double curQty = 0;

	//���ʣ��˴�Ϊ0 ������Ҫȫ���µ�
	//����ȡ��(ʣ������/ʣ�����)��1�����ֵ������СΪһ�֣�����Ҫע����Ŵ���
	if (leftTimes == 0)
		curQty = diffQty;
	else
		curQty = max(1.0, round(abs(diffQty) / leftTimes)) * abs(diffQty) / diffQty;

	//�趨����Ŀ���λ
	_this_target = realPos + curQty;


	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	uint64_t now = TimeUtils::getLocalTimeNow();
	bool isBuy = decimal::gt(diffQty, 0);
	double targetPx = 0;
	//���ݼ۸�ģʽ���ã�ȷ��ί�л�׼�۸�0-���¼ۣ�1-���żۣ�2-���ּ�
	if (_price_mode == 0)
	{
		targetPx = curTick->price();
	}
	else if(_price_mode == 1)
	{
		targetPx = isBuy ? curTick->bidprice(0) : curTick->askprice(0);
	}
	else // if(_price_mode == 2)
	{
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}

	//�����Ҫȫ����������۸�ƫ��5�����Ա���ִ��
	if(bNeedShowHand) //  last showhand time
	{		
		targetPx += _comm_info->getPriceTick() * 5 * (isBuy ? 1 : -1);
	}
	else if(_price_offset != 0)	//��������˼۸�ƫ�ƣ�ҲҪ����һ��
	{
		targetPx += _comm_info->getPriceTick() * _price_offset * (isBuy ? 1 : -1);
	}
	
	//��󷢳�ָ��
	OrderIDs ids;
	if (curQty > 0)
		ids = _ctx->buy(code, targetPx, abs(curQty));
	else
		ids = _ctx->sell(code, targetPx, abs(curQty));

	StdLocker<StdRecurMutex> lock(_mtx_ords);
	for (auto localid : ids)
	{
		_orders[localid] = now;
	}
	_last_fire_time = now;
	_fired_times += 1;

	curTick->release();
}

void WtTWapExeUnit::set_position(const char* stdCode, double newVol)
{
	if (_code.compare(stdCode) != 0)
		return;

	//double diff = newVol - _target_pos;
	if (decimal::eq(newVol, _target_pos))
		return;

	_target_pos = newVol;

	_fired_times = 0;

	do_calc();
}