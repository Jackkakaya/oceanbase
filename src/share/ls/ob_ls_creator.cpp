/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include "common/ob_timeout_ctx.h"
#include "share/ob_share_util.h"
#define USING_LOG_PREFIX SHARE
#include "ob_ls_creator.h"
#include "share/ob_rpc_struct.h" //ObLSCreatorArg, ObSetMemberListArgV2
#include "share/ls/ob_ls_status_operator.h" //ObLSStatusOperator
#include "share/ls/ob_ls_operator.h" //ObLSHistoryOperator
#include "share/ls/ob_ls_table.h"
#include "share/ls/ob_ls_table_operator.h"
#include "share/ls/ob_ls_info.h"
#include "share/ob_tenant_info_proxy.h"
#include "rootserver/ob_root_utils.h" //majority
#include "share/ob_unit_table_operator.h" //ObUnitTableOperator
#include "logservice/leader_coordinator/table_accessor.h"
#include "logservice/palf/palf_base_info.h"//palf::PalfBaseInfo
#include "share/scn.h"
#include "share/ls/ob_ls_life_manager.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::rootserver;
using namespace oceanbase::obrpc;
using namespace oceanbase::palf;

namespace oceanbase
{
namespace share
{
////ObLSReplicaAddr
int ObLSReplicaAddr::init(const common::ObAddr &addr,
           const common::ObReplicaType replica_type,
           const common::ObReplicaProperty &replica_property,
           const uint64_t unit_group_id,
           const uint64_t unit_id,
           const common::ObZone &zone)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!addr.is_valid()
                  || common::REPLICA_TYPE_MAX == replica_type
                  || !replica_property.is_valid()
                  || common::OB_INVALID_ID == unit_group_id
                  || common::OB_INVALID_ID == unit_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(addr), K(replica_type),
        K(replica_property), K(unit_group_id), K(unit_id));
  } else if (OB_FAIL(zone_.assign(zone))) {
    LOG_WARN("failed to assign zone", KR(ret), K(zone));
  } else {
    addr_ = addr;
    replica_type_ = replica_type;
    replica_property_ = replica_property;
    unit_group_id_ = unit_group_id;
    unit_id_ = unit_id;
  }

  return ret;
}


/////////////////////////
bool ObLSCreator::is_valid()
{
  bool bret = true;
  if (OB_INVALID_TENANT_ID == tenant_id_
      || !id_.is_valid()) {
    bret = false;
    LOG_WARN_RET(OB_INVALID_ARGUMENT, "tenant id or log stream id is invalid", K(bret), K_(tenant_id), K_(id));
  }
  return bret;
}

int ObLSCreator::create_sys_tenant_ls(
    const obrpc::ObServerInfoList &rs_list,
    const common::ObIArray<share::ObUnit> &unit_array)
{
  int ret = OB_SUCCESS;
  ObAllTenantInfo tenant_info;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_UNLIKELY(0 >= rs_list.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("rs list is invalid", KR(ret), K(rs_list));
  } else if (OB_UNLIKELY(rs_list.count() != unit_array.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(rs_list), K(unit_array));
  } else if (OB_FAIL(tenant_info.init(OB_SYS_TENANT_ID, share::PRIMARY_TENANT_ROLE))) {
    LOG_WARN("tenant info init failed", KR(ret));
  } else {
    ObLSAddr addr;
    common::ObMemberList member_list;
    const int64_t paxos_replica_num = rs_list.count();
    ObLSReplicaAddr replica_addr;
    const common::ObReplicaProperty replica_property;
    const common::ObReplicaType replica_type = common::REPLICA_TYPE_FULL;
    const common::ObCompatibilityMode compat_mode = MYSQL_MODE;
    const SCN create_scn = SCN::base_scn();//SYS_LS no need create_scn
    palf::PalfBaseInfo palf_base_info;
    common::ObMember arbitration_service;
    common::GlobalLearnerList learner_list;
    for (int64_t i = 0; OB_SUCC(ret) && i < rs_list.count(); ++i) {
      replica_addr.reset();
      if (rs_list.at(i).zone_ != unit_array.at(i).zone_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("zone not match", KR(ret), K(rs_list), K(unit_array));
      } else if (OB_FAIL(replica_addr.init(
              rs_list[i].server_,
              replica_type,
              replica_property,
              0,/* sys ls has no unit group id*/
              unit_array.at(i).unit_id_,
              rs_list.at(i).zone_))) {
        LOG_WARN("failed to init replica addr", KR(ret), K(i), K(rs_list), K(replica_type),
                 K(replica_property), K(unit_array));
      } else if (OB_FAIL(addr.push_back(replica_addr))) {
        LOG_WARN("failed to push back replica addr", KR(ret), K(i), K(addr),
            K(replica_addr), K(rs_list));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(create_ls_(addr, paxos_replica_num, tenant_info,
            create_scn, compat_mode, false/*create_with_palf*/, palf_base_info,
            member_list, arbitration_service, learner_list))) {
      LOG_WARN("failed to create log stream", KR(ret), K_(id), K_(tenant_id),
                                              K(addr), K(paxos_replica_num), K(tenant_info),
                                              K(create_scn), K(compat_mode), K(palf_base_info));
    } else if (OB_FAIL(set_member_list_(member_list, arbitration_service, paxos_replica_num, learner_list))) {
      LOG_WARN("failed to set member list", KR(ret), K(member_list), K(arbitration_service), K(paxos_replica_num));
    }
  }
  return ret;
}

#define REPEAT_CREATE_LS()                     \
  do {                                                                         \
    if (OB_FAIL(ret)) {                        \
    } else if (0 >= member_list.get_member_number()) {                       \
      if (OB_FAIL(do_create_ls_(addr, arbitration_service, status_info, paxos_replica_num, \
              create_scn, compat_mode, member_list, create_with_palf, palf_base_info, learner_list))) {         \
        LOG_WARN("failed to create log stream", KR(ret), K_(id),             \
            K_(tenant_id), K(addr), K(paxos_replica_num),               \
            K(status_info), K(create_scn), K(palf_base_info));                  \
      }                                                                      \
    }                                                                        \
    if (FAILEDx(process_after_has_member_list_(member_list, arbitration_service,   \
            paxos_replica_num, learner_list))) {        \
      LOG_WARN("failed to process after has member list", KR(ret),           \
          K(member_list), K(paxos_replica_num));                        \
    }                                                                        \
  } while(0)

int ObLSCreator::create_user_ls(
    const share::ObLSStatusInfo &status_info,
    const int64_t paxos_replica_num,
    const share::schema::ZoneLocalityIArray &zone_locality,
    const SCN &create_scn,
    const common::ObCompatibilityMode &compat_mode,
    const bool create_with_palf,
    const palf::PalfBaseInfo &palf_base_info)
{
  int ret = OB_SUCCESS;
  const int64_t start_time = ObTimeUtility::current_time(); 
  LOG_INFO("start to create log stream", K_(id), K_(tenant_id));
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_UNLIKELY(!status_info.is_valid()
                         || !id_.is_user_ls()
                         || 0 >= zone_locality.count()
                         || 0 >= paxos_replica_num
                         || !create_scn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(status_info), K_(id), K(zone_locality),
             K(paxos_replica_num), K(create_scn), K(palf_base_info));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", KR(ret));
  } else {
    ObLSAddr addr;
    common::ObMemberList member_list;
    share::ObLSStatusInfo exist_status_info;
    share::ObLSStatusOperator ls_operator;
    ObMember arbitration_service;
    common::GlobalLearnerList learner_list;
    if (status_info.is_duplicate_ls()) {
      if (OB_FAIL(alloc_duplicate_ls_addr_(tenant_id_, zone_locality, addr))) {
        LOG_WARN("failed to alloc duplicate ls addr", KR(ret), K_(tenant_id));
      } else {
        LOG_INFO("finish alloc duplicate ls addr", K_(tenant_id), K(addr));
      }
    } else if (OB_FAIL(alloc_user_ls_addr(tenant_id_, status_info.unit_group_id_,
                                           zone_locality, addr))) {
      LOG_WARN("failed to alloc user ls addr", KR(ret), K(tenant_id_), K(status_info));
    }

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ls_operator.get_ls_init_member_list(tenant_id_, id_, member_list,
            exist_status_info, *proxy_, arbitration_service, learner_list))) {
      LOG_WARN("failed to get ls init member list", KR(ret), K(tenant_id_), K(id_));
    } else if (status_info.ls_is_created()) {
    } else if (status_info.ls_group_id_ != exist_status_info.ls_group_id_
        || status_info.unit_group_id_ != exist_status_info.unit_group_id_) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("exist status not equal to status", KR(ret),
                 K(exist_status_info), K(status_info));
    } else {
      REPEAT_CREATE_LS();
    }
  }
  const int64_t cost = ObTimeUtility::current_time() - start_time;
  LOG_INFO("finish to create log stream", KR(ret), K_(id), K_(tenant_id), K(cost));
  return ret;
}

int ObLSCreator::create_tenant_sys_ls(
    const common::ObZone &primary_zone,
    const share::schema::ZoneLocalityIArray &zone_locality,
    const ObIArray<share::ObResourcePoolName> &pool_list,
    const int64_t paxos_replica_num,
    const common::ObCompatibilityMode &compat_mode,
    const ObString &zone_priority,
    const bool create_with_palf,
    const palf::PalfBaseInfo &palf_base_info)
{
  int ret = OB_SUCCESS;
  LOG_INFO("start to create log stream", K_(id), K_(tenant_id));
  const int64_t start_time = ObTimeUtility::current_time();
  share::ObLSStatusInfo status_info;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_UNLIKELY(primary_zone.is_empty()
                         || 0 >= zone_locality.count()
                         || 0 >= paxos_replica_num
                         || 0 >= pool_list.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(primary_zone), K(zone_locality),
             K(paxos_replica_num), K(pool_list), K(palf_base_info));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", KR(ret));
  } else {
    ObLSAddr addr;
    common::ObMemberList member_list;
    share::ObLSStatusInfo exist_status_info;
    const SCN create_scn = SCN::base_scn();
    share::ObLSStatusOperator ls_operator;
    ObMember arbitration_service;
    common::GlobalLearnerList learner_list;
    ObLSFlag flag(ObLSFlag::NORMAL_FLAG); // TODO: sys ls should be duplicate
    if (OB_FAIL(status_info.init(tenant_id_, id_, 0, share::OB_LS_CREATING, 0,
                                   primary_zone, flag))) {
      LOG_WARN("failed to init ls info", KR(ret), K(id_), K(primary_zone),
          K(tenant_id_), K(flag));
    } else if (OB_FAIL(alloc_sys_ls_addr(tenant_id_, pool_list,
            zone_locality, addr))) {
      LOG_WARN("failed to alloc user ls addr", KR(ret), K(tenant_id_), K(pool_list));
    } else {
      ret = ls_operator.get_ls_init_member_list(tenant_id_, id_, member_list, exist_status_info, *proxy_, arbitration_service, learner_list);
      if (OB_FAIL(ret) && OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("failed to get log stream member list", KR(ret), K_(id), K(tenant_id_));
      } else if (OB_SUCC(ret) && status_info.ls_is_created()) {
      } else {
        if (OB_ENTRY_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
          share::ObLSLifeAgentManager ls_life_agent(*proxy_);
          if (OB_FAIL(ls_life_agent.create_new_ls(status_info, create_scn, zone_priority,
                                                  share::NORMAL_SWITCHOVER_STATUS))) {
            LOG_WARN("failed to create new ls", KR(ret), K(status_info), K(create_scn), K(zone_priority));
          }
        }
        if (OB_SUCC(ret)) {
          REPEAT_CREATE_LS();
        }
      }
    }
  }

  const int64_t cost = ObTimeUtility::current_time() - start_time;
  LOG_INFO("finish to create log stream", KR(ret), K_(id), K_(tenant_id), K(cost));
  return ret;
}

int ObLSCreator::do_create_ls_(const ObLSAddr &addr,
                              ObMember &arbitration_service,
                              const share::ObLSStatusInfo &info,
                              const int64_t paxos_replica_num,
                              const SCN &create_scn,
                              const common::ObCompatibilityMode &compat_mode,
                              ObMemberList &member_list,
                              const bool create_with_palf,
                              const palf::PalfBaseInfo &palf_base_info,
                              common::GlobalLearnerList &learner_list)
{
 int ret = OB_SUCCESS;
 ObAllTenantInfo tenant_info;
 if (OB_UNLIKELY(!is_valid())) {
   ret = OB_INVALID_ARGUMENT;
   LOG_WARN("invalid argument", KR(ret));
 } else if (OB_UNLIKELY(0 >= addr.count() || 0 >= paxos_replica_num ||
                        !info.is_valid()
                        || !create_scn.is_valid())) {
   ret = OB_INVALID_ARGUMENT;
   LOG_WARN("invalid argument", KR(ret), K(paxos_replica_num), K(info),
            K(addr), K(create_scn));
 } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(tenant_id_, proxy_, false, tenant_info))) {
   LOG_WARN("failed to load tenant info", KR(ret), K_(tenant_id));
 } else if (OB_FAIL(create_ls_(addr, paxos_replica_num, tenant_info, create_scn,
                               compat_mode, create_with_palf, palf_base_info, member_list, arbitration_service, learner_list))) {
   LOG_WARN("failed to create log stream", KR(ret), K_(id), K_(tenant_id), K(create_with_palf),
            K(addr), K(paxos_replica_num), K(tenant_info), K(create_scn), K(compat_mode), K(palf_base_info), K(learner_list));
 } else if (OB_FAIL(persist_ls_member_list_(member_list, arbitration_service, learner_list))) {
   LOG_WARN("failed to persist log stream member list", KR(ret),
            K(member_list), K(arbitration_service), K(learner_list));
 }
  return ret;
}

int ObLSCreator::process_after_has_member_list_(
    const common::ObMemberList &member_list,
    const common::ObMember &arbitration_service,
    const int64_t paxos_replica_num,
    const common::GlobalLearnerList &learner_list)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_FAIL(set_member_list_(member_list, arbitration_service, paxos_replica_num, learner_list))) {
    LOG_WARN("failed to set member list", KR(ret), K_(id), K_(tenant_id),
        K(member_list), K(arbitration_service), K(paxos_replica_num), K(learner_list));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", KR(ret));
  } else {
    //create end
    DEBUG_SYNC(BEFORE_PROCESS_AFTER_HAS_MEMBER_LIST);
    share::ObLSStatusOperator ls_operator;
    if (OB_FAIL(ls_operator.update_ls_status(
            tenant_id_, id_, share::OB_LS_CREATING, share::OB_LS_CREATED,
            share::NORMAL_SWITCHOVER_STATUS, *proxy_))) {
      LOG_WARN("failed to update ls status", KR(ret), K(id_));
    } else if (id_.is_sys_ls()) {
      if (OB_FAIL(ls_operator.update_ls_status(
                  tenant_id_, id_, share::OB_LS_CREATED, share::OB_LS_NORMAL,
                  share::NORMAL_SWITCHOVER_STATUS, *proxy_))) {
        LOG_WARN("failed to update ls status", KR(ret), K(id_));
      }
    }
  }
  return ret;
}

int ObLSCreator::create_ls_(const ObILSAddr &addrs,
                           const int64_t paxos_replica_num,
                           const share::ObAllTenantInfo &tenant_info,
                           const SCN &create_scn,
                           const common::ObCompatibilityMode &compat_mode,
                           const bool create_with_palf,
                           const palf::PalfBaseInfo &palf_base_info,
                           common::ObMemberList &member_list,
                           common::ObMember &arbitration_service,
                           common::GlobalLearnerList &learner_list)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_UNLIKELY(0 >= addrs.count()
                         || 0 >= paxos_replica_num
                         || rootserver::majority(paxos_replica_num) > addrs.count()
                         || !tenant_info.is_valid()
                         || !create_scn.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(addrs), K(paxos_replica_num), K(tenant_info),
        K(create_scn));
  } else {
    ObTimeoutCtx ctx;
    if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, GCONF.rpc_timeout))) {
      LOG_WARN("fail to set timeout ctx", KR(ret));
    } else {
      obrpc::ObCreateLSArg arg;
      int64_t rpc_count = 0;
      int tmp_ret = OB_SUCCESS;
      ObArray<int> return_code_array;
      lib::Worker::CompatMode new_compat_mode = compat_mode == ORACLE_MODE ?
                                         lib::Worker::CompatMode::ORACLE :
                                         lib::Worker::CompatMode::MYSQL;

      for (int64_t i = 0; OB_SUCC(ret) && i < addrs.count(); ++i) {
        arg.reset();
        const ObLSReplicaAddr &addr = addrs.at(i);
        rpc_count++;
        if (OB_FAIL(arg.init(tenant_id_, id_, addr.replica_type_,
                addr.replica_property_, tenant_info, create_scn, new_compat_mode,
                create_with_palf, palf_base_info))) {
          LOG_WARN("failed to init create log stream arg", KR(ret), K(addr), K(create_with_palf),
              K_(id), K_(tenant_id), K(tenant_info), K(create_scn), K(new_compat_mode), K(palf_base_info));
        } else if (OB_TMP_FAIL(create_ls_proxy_.call(addr.addr_, ctx.get_timeout(),
                GCONF.cluster_id, tenant_id_, arg))) {
          LOG_WARN("failed to all async rpc", KR(tmp_ret), K(addr), K(ctx.get_timeout()),
              K(arg), K(tenant_id_));
        }
      }
      //wait all
      if (OB_TMP_FAIL(create_ls_proxy_.wait_all(return_code_array))) {
        ret = OB_SUCC(ret) ? tmp_ret : ret;
        LOG_WARN("failed to wait all async rpc", KR(ret), KR(tmp_ret), K(rpc_count));
      }
      if (FAILEDx(check_create_ls_result_(rpc_count, paxos_replica_num, return_code_array, member_list, learner_list))) {
        LOG_WARN("failed to check ls result", KR(ret), K(rpc_count), K(paxos_replica_num), K(return_code_array), K(learner_list));
      }

    }
  }
  return ret;
}


int ObLSCreator::check_create_ls_result_(const int64_t rpc_count,
                                        const int64_t paxos_replica_num,
                                        const ObIArray<int> &return_code_array,
                                        common::ObMemberList &member_list,
                                        common::GlobalLearnerList &learner_list)
{
  int ret = OB_SUCCESS;
  member_list.reset();
  learner_list.reset();
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (rpc_count != return_code_array.count()
             || rpc_count !=  create_ls_proxy_.get_results().count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("rpc count not equal to result count", KR(ret), K(rpc_count),
          K(return_code_array), "arg count", create_ls_proxy_.get_args().count());
  } else {
    const int64_t timestamp = 1;
    for (int64_t i = 0; OB_SUCC(ret) && i < return_code_array.count(); ++i) {
      if (OB_SUCCESS != return_code_array.at(i)) {
        LOG_WARN("rpc is failed", KR(ret), K(return_code_array.at(i)), K(i));
      } else {
        const auto *result = create_ls_proxy_.get_results().at(i);
        if (OB_ISNULL(result)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("result is null", KR(ret), K(i));
        } else if (OB_SUCCESS != result->get_result()) {
          LOG_WARN("rpc is failed", KR(ret), K(*result), K(i));
        } else {
          ObAddr addr;
          if (result->get_addr().is_valid()) {
            addr = result->get_addr();
          } else if (create_ls_proxy_.get_dests().count() == create_ls_proxy_.get_results().count()) {
            //one by one match
            addr = create_ls_proxy_.get_dests().at(i);
          }
          //TODO other replica type
          //can not get replica type from arg, arg and result is not match
          if (OB_FAIL(ret)) {
          } else if (OB_UNLIKELY(!addr.is_valid())) {
            ret = OB_NEED_RETRY;
            LOG_WARN("addr is invalid, ls create failed", KR(ret), K(addr));
          } else if (result->get_replica_type() == REPLICA_TYPE_FULL) {
            if (OB_FAIL(member_list.add_member(ObMember(addr, timestamp)))) {
              LOG_WARN("failed to add member", KR(ret), K(addr));
            }
          } else if (result->get_replica_type() == REPLICA_TYPE_READONLY) {
            if (OB_FAIL(learner_list.add_learner(ObMember(addr, timestamp)))) {
              LOG_WARN("failed to add member", KR(ret), K(addr));
            }
          }
          LOG_TRACE("create ls result", KR(ret), K(i), K(addr), KPC(result), K(rpc_count));
        }
      }
    }
    if (rootserver::majority(paxos_replica_num) > member_list.get_member_number()) {
      ret = OB_REPLICA_NUM_NOT_ENOUGH;
      LOG_WARN("success count less than majority", KR(ret), K(paxos_replica_num),
               K(member_list));
    }
  }
  return ret;
}

int ObLSCreator::persist_ls_member_list_(const common::ObMemberList &member_list,
                                         const ObMember &arb_member,
                                         const common::GlobalLearnerList &learner_list)
{
  int ret = OB_SUCCESS;
  DEBUG_SYNC(BEFORE_SET_LS_MEMBER_LIST);
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_UNLIKELY(!member_list.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("member list is invalid", KR(ret), K(member_list));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is null", KR(ret));
  } else {
    share::ObLSStatusOperator ls_operator;
    if (OB_FAIL(ls_operator.update_init_member_list(tenant_id_, id_, member_list, *proxy_, arb_member, learner_list))) {
      LOG_WARN("failed to insert ls", KR(ret), K(member_list), K(arb_member), K(learner_list));
    }
  }
  return ret;

}

int ObLSCreator::check_member_list_and_learner_list_all_in_meta_table_(
    const common::ObMemberList &member_list,
    const common::GlobalLearnerList &learner_list)
{
  int ret = OB_SUCCESS;
  bool has_replica_only_in_member_list_or_learner_list = true;
  ObLSInfo ls_info_to_check;
  const int64_t retry_interval_us = 1000l * 1000l; // 1s
  ObTimeoutCtx ctx;
  if (OB_ISNULL(GCTX.lst_operator_)
      || OB_UNLIKELY(!is_valid() || !member_list.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(member_list));
  } else if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, GCONF.internal_sql_execute_timeout))) {
    LOG_WARN("failed to set default timeout", KR(ret));
  } else {
    while (OB_SUCC(ret) && has_replica_only_in_member_list_or_learner_list) {
      has_replica_only_in_member_list_or_learner_list = false;
      if (ctx.is_timeouted()) {
        ret = OB_TIMEOUT;
        LOG_WARN("wait member list all reported to meta table timeout", KR(ret), K(member_list), K_(tenant_id), K_(id));
      } else if (OB_FAIL(GCTX.lst_operator_->get(GCONF.cluster_id, tenant_id_, id_, share::ObLSTable::DEFAULT_MODE, ls_info_to_check))) {
        LOG_WARN("fail to get ls info", KR(ret), K_(tenant_id), K_(id));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < member_list.get_member_number(); ++i) {
          const share::ObLSReplica *replica = nullptr;
          common::ObAddr server;
          if (OB_FAIL(member_list.get_server_by_index(i, server))) {
            LOG_WARN("fail to get server by index", KR(ret), K(i), K(member_list));
          } else {
            int tmp_ret = ls_info_to_check.find(server, replica);
            if (OB_SUCCESS == tmp_ret) {
              // replica exists, bypass
            } else {
              has_replica_only_in_member_list_or_learner_list = true;
              LOG_INFO("has replica only in member list", KR(tmp_ret), K(member_list), K(ls_info_to_check), K(i), K(server));
              break;
            }
          }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < learner_list.get_member_number(); ++i) {
          const share::ObLSReplica *replica = nullptr;
          common::ObAddr server;
          if (OB_FAIL(learner_list.get_server_by_index(i, server))) {
            LOG_WARN("fail to get server by index", KR(ret), K(i), K(learner_list));
          } else {
            int tmp_ret = ls_info_to_check.find(server, replica);
            if (OB_SUCCESS == tmp_ret) {
              // replica exists, bypass
            } else {
              has_replica_only_in_member_list_or_learner_list = true;
              LOG_INFO("has replica only in learner list", KR(tmp_ret), K(learner_list), K(ls_info_to_check), K(i), K(server));
              break;
            }
          }
        }
        if (OB_SUCC(ret) && has_replica_only_in_member_list_or_learner_list) {
          ob_usleep(retry_interval_us);
        }
      }
    }
  }
  return ret;
}

int ObLSCreator::set_member_list_(const common::ObMemberList &member_list,
                                  const common::ObMember &arbitration_service,
                                  const int64_t paxos_replica_num,
                                  const common::GlobalLearnerList &learner_list)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (OB_UNLIKELY(!member_list.is_valid()
                         || member_list.get_member_number() < rootserver::majority(paxos_replica_num))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(member_list), K(paxos_replica_num));
  } else if (!is_sys_tenant(tenant_id_) && OB_FAIL(check_member_list_and_learner_list_all_in_meta_table_(member_list, learner_list))) {
    LOG_WARN("fail to check member_list all in meta table", KR(ret), K(member_list), K(learner_list), K_(tenant_id), K_(id));
  } else {
    ObTimeoutCtx ctx;
    if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, GCONF.rpc_timeout))) {
      LOG_WARN("fail to set timeout ctx", KR(ret));
    } else {
      int64_t rpc_count = 0;
      int tmp_ret = OB_SUCCESS;
      ObArray<int> return_code_array;
      for (int64_t i = 0; OB_SUCC(ret) && i < member_list.get_member_number(); ++i) {
        ObAddr addr;
        rpc_count++;
        ObSetMemberListArgV2 arg;
        if (OB_FAIL(arg.init(tenant_id_, id_, paxos_replica_num, member_list, arbitration_service, learner_list))) {
          LOG_WARN("failed to init set member list arg", KR(ret), K_(id), K_(tenant_id),
              K(paxos_replica_num), K(member_list), K(arbitration_service), K(learner_list));
        } else if (OB_FAIL(member_list.get_server_by_index(i, addr))) {
          LOG_WARN("failed to get member by index", KR(ret), K(i), K(member_list));
        } else if (OB_TMP_FAIL(set_member_list_proxy_.call(addr, ctx.get_timeout(),
                GCONF.cluster_id, tenant_id_, arg))) {
          LOG_WARN("failed to set member list", KR(tmp_ret), K(ctx.get_timeout()), K(arg),
              K(tenant_id_));
        }
      }

      if (OB_TMP_FAIL(set_member_list_proxy_.wait_all(return_code_array))) {
        ret = OB_SUCC(ret) ? tmp_ret : ret;
        LOG_WARN("failed to wait all async rpc", KR(ret), KR(tmp_ret), K(rpc_count));

      }
      if (FAILEDx(check_set_memberlist_result_(rpc_count, return_code_array, paxos_replica_num))) {
        LOG_WARN("failed to check set member liset result", KR(ret), K(rpc_count),
            K(paxos_replica_num), K(return_code_array));
      }
    }
  }
  return ret;
}

int ObLSCreator::check_set_memberlist_result_(const int64_t rpc_count,
                            const ObIArray<int> &return_code_array,
                            const int64_t paxos_replica_num)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret));
  } else if (rpc_count != return_code_array.count()
             || rpc_count !=  set_member_list_proxy_.get_results().count()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("rpc count not equal to result count", KR(ret), K(rpc_count),
          K(return_code_array), "arg count", set_member_list_proxy_.get_args().count());
  } else {
    int64_t success_cnt = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < return_code_array.count(); ++i) {
      if (OB_SUCCESS != return_code_array.at(i)) {
        LOG_WARN("rpc is failed", KR(ret), K(return_code_array.at(i)), K(i));
      } else {
        const auto *result = set_member_list_proxy_.get_results().at(i);
        if (OB_ISNULL(result)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("result is null", KR(ret), K(i));
        } else if (OB_SUCCESS != result->get_result()) {
          LOG_WARN("rpc is failed", KR(ret), K(*result), K(i));
        } else {
          success_cnt++;
        }
      }
    }
    if (rootserver::majority(paxos_replica_num) > success_cnt) {
      ret = OB_REPLICA_NUM_NOT_ENOUGH;
      LOG_WARN("success count less than majority", KR(ret), K(success_cnt),
               K(paxos_replica_num));
    }
  }
  return ret;
}

// OceanBase 4.0 interface
int ObLSCreator::alloc_sys_ls_addr(
    const uint64_t tenant_id,
    const ObIArray<share::ObResourcePoolName> &pools,
    const share::schema::ZoneLocalityIArray &zone_locality_array,
    ObILSAddr &ls_addr)
{
  int ret = OB_SUCCESS;
  common::ObArray<share::ObUnit> unit_info_array;
  ObUnitTableOperator unit_operator;

  if (OB_UNLIKELY(OB_INVALID_ID == tenant_id
                         || pools.count() <= 0
                         || zone_locality_array.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(pools), K(zone_locality_array));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy ptr is null", KR(ret));
  } else if (OB_FAIL(unit_operator.init(*proxy_))) {
    LOG_WARN("unit operator init failed", KR(ret));
  } else if (OB_FAIL(unit_operator.get_units_by_resource_pools(pools, unit_info_array))) {
    LOG_WARN("fail to get unit infos", KR(ret), K(pools));
  } else {
    ls_addr.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < zone_locality_array.count(); ++i) {
      const share::ObZoneReplicaAttrSet &zone_locality = zone_locality_array.at(i);
      const bool is_sys_ls = true;
      ObLSReplicaAddr replica_addr;
      if (OB_FAIL(alloc_zone_ls_addr(
              is_sys_ls, zone_locality, unit_info_array, replica_addr))) {
        LOG_WARN("fail to alloc zone ls addr",
                 KR(ret), K(zone_locality), K(unit_info_array));
      } else if (OB_FAIL(ls_addr.push_back(replica_addr))) {
        LOG_WARN("fail to push back", KR(ret));
      }
    }
  }
  return ret;
}

int ObLSCreator::alloc_user_ls_addr(
    const uint64_t tenant_id,
    const uint64_t unit_group_id,
    const share::schema::ZoneLocalityIArray &zone_locality_array,
    ObILSAddr &ls_addr)
{
  int ret = OB_SUCCESS;
  ObUnitTableOperator unit_operator;

  common::ObArray<share::ObUnit> unit_info_array;
  if (OB_UNLIKELY(OB_INVALID_ID == tenant_id
                         || 0 == unit_group_id
                         || OB_INVALID_ID == unit_group_id
                         || zone_locality_array.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id),
             K(unit_group_id), K(zone_locality_array));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy ptr is null", KR(ret));
  } else if (OB_FAIL(unit_operator.init(*proxy_))) {
    LOG_WARN("unit operator init failed", KR(ret));
  } else if (OB_FAIL(unit_operator.get_units_by_unit_group_id(unit_group_id, unit_info_array))) {
    LOG_WARN("fail to get unit group", KR(ret), K(tenant_id), K(unit_group_id));
  } else {
    ls_addr.reset();
    for (int64_t i = 0; OB_SUCC(ret) && i < zone_locality_array.count(); ++i) {
      const share::ObZoneReplicaAttrSet &zone_locality = zone_locality_array.at(i);
      const bool is_sys_ls = false;
      ObLSReplicaAddr replica_addr;
      if (OB_FAIL(alloc_zone_ls_addr(
              is_sys_ls, zone_locality, unit_info_array, replica_addr))) {
        LOG_WARN("fail to alloc zone ls addr",
                 KR(ret), K(zone_locality), K(unit_info_array));
      } else if (OB_FAIL(ls_addr.push_back(replica_addr))) {
        LOG_WARN("fail to push back", KR(ret));
      }
    }
  }
  return ret;
}

int ObLSCreator::alloc_duplicate_ls_addr_(
    const uint64_t tenant_id,
    const share::schema::ZoneLocalityIArray &zone_locality_array,
    ObILSAddr &ls_addr)
{
  //TODO: alloc_sys_ls_addr and alloc_duplicate_ls_addr should merge into one function
  int ret = OB_SUCCESS;
  ObUnitTableOperator unit_operator;
  common::ObArray<share::ObUnit> unit_info_array;

  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id
                  || zone_locality_array.count() <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(zone_locality_array));
  } else if (OB_ISNULL(proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("proxy ptr is null", KR(ret));
  } else if (OB_FAIL(unit_operator.init(*proxy_))) {
    LOG_WARN("unit operator init failed", KR(ret));
  } else if (OB_FAIL(unit_operator.get_units_by_tenant(tenant_id, unit_info_array))) {
    LOG_WARN("fail to get unit info array", KR(ret), K(tenant_id));
  } else {
    ls_addr.reset();
    const bool is_duplicate_ls = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < zone_locality_array.count(); ++i) {
      const share::ObZoneReplicaAttrSet &zone_locality = zone_locality_array.at(i);
      ObLSReplicaAddr replica_addr;
      if (OB_FAIL(alloc_zone_ls_addr(is_duplicate_ls, zone_locality, unit_info_array, replica_addr))) {
        LOG_WARN("fail to alloc zone ls addr", KR(ret), K(zone_locality), K(unit_info_array));
      } else if (OB_FAIL(ls_addr.push_back(replica_addr))) {
        LOG_WARN("fail to push back", KR(ret));
      } else if (OB_FAIL(compensate_zone_readonly_replica_(
                             zone_locality,
                             replica_addr,
                             unit_info_array,
                             ls_addr))) {
        LOG_WARN("fail to compensate readonly replica", KR(ret),
                 K(zone_locality), K(replica_addr), K(ls_addr));
      }
    }
  }
  return ret;
}

int ObLSCreator::compensate_zone_readonly_replica_(
    const share::ObZoneReplicaAttrSet &zlocality,
    const ObLSReplicaAddr &exclude_replica,
    const common::ObIArray<share::ObUnit> &unit_info_array,
    ObILSAddr &ls_addr)
{
  int ret = OB_SUCCESS;
  const common::ObZone &locality_zone = zlocality.zone_;
  const uint64_t unit_group_id = 0; // duplicate log stream
  if (OB_UNLIKELY(0 >= unit_info_array.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(unit_info_array));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < unit_info_array.count(); ++i) {
      const share::ObUnit &unit = unit_info_array.at(i);
      if (locality_zone != unit.zone_) {
        // not match
      } else if (exclude_replica.unit_id_ == unit.unit_id_) {
        // already exists in ls_addr
      } else if (ObUnit::UNIT_STATUS_DELETING == unit.status_) {
        // unit may be deleting
        LOG_TRACE("unit is not active", K(unit));
      } else {
        ObLSReplicaAddr ls_replica_addr;
        const int64_t m_percent = 100;
        ObReplicaProperty replica_property;
        replica_property.set_memstore_percent(m_percent);
        if (OB_FAIL(ls_replica_addr.init(
                      unit.server_,
                      ObReplicaType::REPLICA_TYPE_READONLY,
                      replica_property,
                      unit_group_id,
                      unit.unit_id_,
                      locality_zone))) {
          LOG_WARN("fail to init ls replica addr", KR(ret), K(unit), K(replica_property),
                   K(unit_group_id), K(locality_zone));
        } else if (OB_FAIL(ls_addr.push_back(ls_replica_addr))) {
          LOG_WARN("fail to push back", KR(ret), K(ls_replica_addr));
        }
      }
    }
  }
  return ret;
}

int ObLSCreator::alloc_zone_ls_addr(
    const bool is_sys_ls,
    const share::ObZoneReplicaAttrSet &zlocality,
    const common::ObIArray<share::ObUnit> &unit_info_array,
    ObLSReplicaAddr &ls_replica_addr)
{
  int ret = OB_SUCCESS;

    bool found = false;
    const common::ObZone &locality_zone = zlocality.zone_;
    ls_replica_addr.reset();
    for (int64_t i = 0; !found && OB_SUCC(ret) && i < unit_info_array.count(); ++i) {
      const share::ObUnit &unit = unit_info_array.at(i);
      const uint64_t unit_group_id = is_sys_ls ? 0 : unit.unit_group_id_;
      if (locality_zone != unit.zone_) {
        // not match
      } else {
        found = true;
        if (zlocality.replica_attr_set_.get_full_replica_attr_array().count() > 0) {
          const int64_t m_percent = zlocality.replica_attr_set_
                                             .get_full_replica_attr_array().at(0)
                                             .memstore_percent_;
          ObReplicaProperty replica_property;
          replica_property.set_memstore_percent(m_percent);
          if (OB_FAIL(ls_replica_addr.init(
                  unit.server_,
                  ObReplicaType::REPLICA_TYPE_FULL,
                  replica_property,
                  unit_group_id,
                  unit.unit_id_,
                  locality_zone))) {
            LOG_WARN("fail to init ls replica addr",
                     KR(ret), K(unit), K(replica_property), K(unit_group_id),
                     K(locality_zone));
          }
        } else if (zlocality.replica_attr_set_.get_logonly_replica_attr_array().count() > 0) {
          const int64_t m_percent = zlocality.replica_attr_set_
                                             .get_logonly_replica_attr_array().at(0)
                                             .memstore_percent_;
          ObReplicaProperty replica_property;
          replica_property.set_memstore_percent(m_percent);
          if (OB_FAIL(ls_replica_addr.init(
                  unit.server_,
                  ObReplicaType::REPLICA_TYPE_LOGONLY,
                  replica_property,
                  unit_group_id,
                  unit.unit_id_,
                  locality_zone))) {
            LOG_WARN("fail to init ls replica addr",
                     KR(ret), K(unit), K(replica_property), K(unit_group_id),
                     K(locality_zone));
          }
        } else if (zlocality.replica_attr_set_
                       .get_encryption_logonly_replica_attr_array()
                       .count() > 0) {
          const int64_t m_percent = zlocality.replica_attr_set_
                                             .get_encryption_logonly_replica_attr_array().at(0)
                                             .memstore_percent_;
          ObReplicaProperty replica_property;
          replica_property.set_memstore_percent(m_percent);
          if (OB_FAIL(ls_replica_addr.init(
                  unit.server_,
                  ObReplicaType::REPLICA_TYPE_ENCRYPTION_LOGONLY,
                  replica_property,
                  unit_group_id,
                  unit.unit_id_,
                  locality_zone))) {
            LOG_WARN("fail to init ls replica addr",
                     KR(ret), K(unit), K(replica_property), K(unit_group_id),
                     K(locality_zone));
          }
        } else if (zlocality.replica_attr_set_.get_readonly_replica_attr_array().count() > 0) {
          const int64_t m_percent = zlocality.replica_attr_set_
                                             .get_readonly_replica_attr_array().at(0)
                                             .memstore_percent_;
          ObReplicaProperty replica_property;
          replica_property.set_memstore_percent(m_percent);
          if (OB_FAIL(ls_replica_addr.init(
                  unit.server_,
                  ObReplicaType::REPLICA_TYPE_READONLY,
                  replica_property,
                  unit_group_id,
                  unit.unit_id_,
                  locality_zone))) {
            LOG_WARN("fail to init ls replica addr",
                     KR(ret), K(unit), K(replica_property), K(unit_group_id),
                     K(locality_zone));
          }
        } else {  // zone locality shall has a paxos replica in 4.0 by
                  // now(2021.10.25)
          ret = OB_NOT_SUPPORTED;
          LOG_WARN("this zone locality not supported", KR(ret), K(zlocality));
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "this zone locality");
        }
      }
    }
    if (OB_FAIL(ret)) {
      // failed
    } else if (!found) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_WARN("locality zone not found", KR(ret), K(zlocality), K(unit_info_array));
    }
  return ret;
}

}
}
